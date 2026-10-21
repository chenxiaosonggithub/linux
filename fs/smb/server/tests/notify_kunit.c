// SPDX-License-Identifier: GPL-2.0-or-later
/* KUnit tests for ksmbd notify. */

#include <kunit/test.h>
#include <linux/slab.h>
#include <linux/uio.h>

#include "../../common/smb2status.h"
#include "../ksmbd_work.h"
#include "../notify.h"
#include "../smb_common.h"
#include "../vfs_cache.h"

struct ksmbd_notify_test_ctx {
	struct ksmbd_notify notify;
	struct ksmbd_file fp;
};

static const struct qstr ksmbd_notify_test_name = QSTR_INIT("entry", 5);

static int ksmbd_notify_test_init(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	spin_lock_init(&ctx->notify.lock);
	INIT_LIST_HEAD(&ctx->notify.events);
	ctx->notify.fp = &ctx->fp;
	ctx->notify.filter = FILE_NOTIFY_CHANGE_FILE_NAME;
	ctx->notify.mask = KSMBD_NOTIFY_NAME_EVENT_MASK;
	return 0;
}

static void ksmbd_notify_test_exit(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;

	ksmbd_notify_free_events_test(notify, &notify->events);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
}

static struct ksmbd_notify_event *
ksmbd_notify_test_alloc(struct ksmbd_notify *notify)
{
	return ksmbd_notify_alloc_event_test(notify, FILE_ACTION_ADDED,
					     &ksmbd_notify_test_name, GFP_KERNEL);
}

static void ksmbd_notify_test_fill(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;
	size_t size = kmalloc_size_roundup(sizeof(*event) +
					 ksmbd_notify_test_name.len + 1);

	while (size <= KSMBD_NOTIFY_MAX_BYTES - notify->event_bytes) {
		event = ksmbd_notify_test_alloc(notify);
		KUNIT_ASSERT_NOT_NULL(test, event);
		ksmbd_notify_queue_event_test(notify, event);
	}
}

static void ksmbd_notify_test_queue_limit(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;

	ksmbd_notify_test_fill(test);
	KUNIT_EXPECT_FALSE(test, notify->overflow);
	KUNIT_EXPECT_LE(test, notify->event_bytes,
			(size_t)KSMBD_NOTIFY_MAX_BYTES);
	event = ksmbd_notify_test_alloc(notify);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
	KUNIT_EXPECT_TRUE(test, notify->overflow);
	KUNIT_EXPECT_TRUE(test, list_empty(&notify->events));
	KUNIT_EXPECT_EQ(test, notify->num_events, 0U);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);

	/* No more allocations until a request consumes the overflow. */
	event = ksmbd_notify_test_alloc(notify);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
}

static void ksmbd_notify_test_oversized_event(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct qstr name = QSTR_INIT("", KSMBD_NOTIFY_MAX_BYTES);
	struct ksmbd_notify_event *event;

	/* The size check must reject this before trying to copy the name. */
	event = ksmbd_notify_alloc_event_test(&ctx->notify, FILE_ACTION_ADDED,
					      &name, GFP_KERNEL);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(&ctx->notify, event);
	KUNIT_EXPECT_TRUE(test, ctx->notify.overflow);
	KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, (size_t)0);
}

struct ksmbd_notify_access_case {
	const char *name;
	u32 mask;
	int data_type;
	bool modified;
};

static const struct ksmbd_notify_access_case ksmbd_notify_access_cases[] = {
	{ "file_read", FS_ACCESS, FSNOTIFY_EVENT_PATH, false },
	{ "directory_read", FS_ACCESS | FS_ISDIR, FSNOTIFY_EVENT_PATH, false },
	{ "file_atime", FS_ACCESS, FSNOTIFY_EVENT_DENTRY, true },
	{ "directory_atime", FS_ACCESS | FS_ISDIR, FSNOTIFY_EVENT_DENTRY, true },
	{ "file_write", FS_MODIFY, FSNOTIFY_EVENT_PATH, true },
	{ "file_attributes", FS_ATTRIB, FSNOTIFY_EVENT_DENTRY, true },
	{ "read_and_write", FS_ACCESS | FS_MODIFY, FSNOTIFY_EVENT_PATH, true },
};

static void ksmbd_notify_access_desc(const struct ksmbd_notify_access_case *param,
				     char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(ksmbd_notify_access, ksmbd_notify_access_cases,
		  ksmbd_notify_access_desc);

static void ksmbd_notify_test_access(struct kunit *test)
{
	const struct ksmbd_notify_access_case *param = test->param_value;
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;
	struct {
		struct fsnotify_group group;
		struct fsnotify_mark mark;
		struct file file;
		struct inode parent;
		struct inode child;
		struct dentry dentry;
	} *fs_ctx;
	struct fsnotify_iter_info iter_info = {};
	struct path path = {};
	const void *data;
	int ret;

	fs_ctx = kunit_kzalloc(test, sizeof(*fs_ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fs_ctx);
	fs_ctx->group.private = notify;
	fs_ctx->file.f_inode = &fs_ctx->parent;
	fs_ctx->dentry.d_inode = &fs_ctx->child;
	ctx->fp.filp = &fs_ctx->file;
	path.dentry = &fs_ctx->dentry;
	iter_info.marks[FSNOTIFY_ITER_TYPE_PARENT] = &fs_ctx->mark;
	fsnotify_iter_set_report_type(&iter_info, FSNOTIFY_ITER_TYPE_PARENT);
	notify->filter = FILE_NOTIFY_CHANGE_LAST_ACCESS |
			 FILE_NOTIFY_CHANGE_LAST_WRITE;
	notify->mask = FS_ACCESS | FS_ATTRIB | FS_MODIFY;
	data = param->data_type == FSNOTIFY_EVENT_PATH ?
		(const void *)&path : &fs_ctx->dentry;

	ret = ksmbd_notify_handle_event_test(&fs_ctx->group,
					     param->mask | FS_EVENT_ON_CHILD,
					     data, param->data_type, &fs_ctx->parent,
					     &ksmbd_notify_test_name, 0, &iter_info);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, notify->overflow);
	KUNIT_EXPECT_EQ(test, notify->num_events, param->modified ? 1U : 0U);
	if (!param->modified) {
		KUNIT_EXPECT_TRUE(test, list_empty(&notify->events));
		KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
		return;
	}

	KUNIT_ASSERT_FALSE(test, list_empty(&notify->events));
	event = list_first_entry(&notify->events, struct ksmbd_notify_event, list);
	KUNIT_EXPECT_EQ(test, event->action, (u32)FILE_ACTION_MODIFIED);
	KUNIT_EXPECT_STREQ(test, event->name, ksmbd_notify_test_name.name);
}

static void notify_test_expect_list(struct kunit *test, struct list_head *events,
				   const char * const *names, const u32 *actions,
				   unsigned int count)
{
	struct ksmbd_notify_event *event;
	unsigned int i = 0;

	list_for_each_entry(event, events, list) {
		KUNIT_ASSERT_LT(test, i, count);
		KUNIT_EXPECT_STREQ(test, event->name, names[i]);
		KUNIT_EXPECT_EQ(test, event->action, actions[i]);
		i++;
	}
	KUNIT_EXPECT_EQ(test, i, count);
}

struct notify_filter_case {
	const char *name;
	u32 filter;
	u32 mask;
	bool accepted;
};

static const struct notify_filter_case notify_filter_cases[] = {
	{ "file_create", FILE_NOTIFY_CHANGE_FILE_NAME, FS_CREATE, true },
	{ "file_delete", FILE_NOTIFY_CHANGE_FILE_NAME, FS_DELETE, true },
	{ "directory_create", FILE_NOTIFY_CHANGE_DIR_NAME, FS_CREATE | FS_ISDIR, true },
	{ "directory_delete", FILE_NOTIFY_CHANGE_DIR_NAME, FS_DELETE | FS_ISDIR, true },
	{ "file_filter_directory_create", FILE_NOTIFY_CHANGE_FILE_NAME,
	  FS_CREATE | FS_ISDIR, false },
	{ "file_filter_directory_delete", FILE_NOTIFY_CHANGE_FILE_NAME,
	  FS_DELETE | FS_ISDIR, false },
	{ "directory_filter_file_create", FILE_NOTIFY_CHANGE_DIR_NAME, FS_CREATE, false },
	{ "directory_filter_file_delete", FILE_NOTIFY_CHANGE_DIR_NAME, FS_DELETE, false },
	{ "both_file", FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
	  FS_CREATE, true },
	{ "both_directory", FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
	  FS_CREATE | FS_ISDIR, true },
	{ "empty_filter", 0, FS_CREATE, false },
};

static void notify_filter_desc(const struct notify_filter_case *param, char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_filter, notify_filter_cases, notify_filter_desc);

static void notify_test_name_filter(struct kunit *test)
{
	const struct notify_filter_case *param = test->param_value;
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	static const char * const names[] = { "entry" };
	u32 actions[] = { param->mask & FS_DELETE ? FILE_ACTION_REMOVED : FILE_ACTION_ADDED };

	notify->filter = param->filter;
	notify->mask = KSMBD_NOTIFY_NAME_EVENT_MASK;
	KUNIT_EXPECT_EQ(test, ksmbd_notify_handle_inode_event_test(notify, param->mask,
				NULL, NULL, &ksmbd_notify_test_name, 0), 0);
	KUNIT_EXPECT_EQ(test, notify->num_events, param->accepted ? 1U : 0U);
	KUNIT_EXPECT_FALSE(test, notify->overflow);
	notify_test_expect_list(test, &notify->events, names, actions,
				param->accepted ? 1 : 0);
	if (!param->accepted)
		KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
}

static void notify_test_filtered_full_queue(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	const u32 masks[] = { FS_CREATE, FS_DELETE, FS_MOVED_FROM, FS_MOVED_TO,
			     FS_MODIFY, FS_OPEN };
	size_t bytes;
	unsigned int count, i;

	ksmbd_notify_test_fill(test);
	bytes = notify->event_bytes;
	count = notify->num_events;
	notify->filter = FILE_NOTIFY_CHANGE_DIR_NAME;
	notify->mask = KSMBD_NOTIFY_NAME_EVENT_MASK;
	for (i = 0; i < ARRAY_SIZE(masks); i++) {
		KUNIT_EXPECT_EQ(test, ksmbd_notify_handle_inode_event_test(notify,
				masks[i], NULL, NULL, &ksmbd_notify_test_name, i + 1), 0);
		KUNIT_EXPECT_FALSE(test, notify->overflow);
		KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
		KUNIT_EXPECT_EQ(test, notify->num_events, count);
	}
}

struct notify_scope_case {
	const char *name;
	int mark_type;
	bool self;
	bool accepted;
};

static const struct notify_scope_case notify_scope_cases[] = {
	{ "parent_mark", FSNOTIFY_ITER_TYPE_PARENT, false, true },
	{ "inode_mark", FSNOTIFY_ITER_TYPE_INODE, false, true },
	{ "superblock_only", FSNOTIFY_ITER_TYPE_SB, false, false },
	{ "no_mark", -1, false, false },
	{ "self_inode_mark", FSNOTIFY_ITER_TYPE_INODE, true, false },
	{ "self_parent_mark", FSNOTIFY_ITER_TYPE_PARENT, true, false },
};

static void notify_scope_desc(const struct notify_scope_case *param, char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_scope, notify_scope_cases, notify_scope_desc);

static void notify_test_nonrecursive_scope(struct kunit *test)
{
	const struct notify_scope_case *param = test->param_value;
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct {
		struct fsnotify_group group;
		struct fsnotify_mark mark;
		struct file file;
		struct inode root, child;
		struct dentry dentry;
	} *fs;
	struct fsnotify_iter_info iter = {};
	static const char * const names[] = { "entry" };
	u32 actions[] = { FILE_ACTION_MODIFIED };

	fs = kunit_kzalloc(test, sizeof(*fs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fs);
	fs->group.private = &ctx->notify;
	fs->file.f_inode = &fs->root;
	fs->dentry.d_inode = param->self ? &fs->root : &fs->child;
	ctx->fp.filp = &fs->file;
	ctx->notify.filter = FILE_NOTIFY_CHANGE_LAST_WRITE;
	ctx->notify.mask = FS_MODIFY;
	if (param->mark_type >= 0) {
		iter.marks[param->mark_type] = &fs->mark;
		fsnotify_iter_set_report_type(&iter, param->mark_type);
	}
	KUNIT_EXPECT_EQ(test, ksmbd_notify_handle_event_test(&fs->group,
			FS_MODIFY | FS_EVENT_ON_CHILD, &fs->dentry,
			FSNOTIFY_EVENT_DENTRY, &fs->root, &ksmbd_notify_test_name,
			0, &iter), 0);
	KUNIT_EXPECT_FALSE(test, ctx->notify.overflow);
	KUNIT_EXPECT_EQ(test, ctx->notify.num_events, param->accepted ? 1U : 0U);
	notify_test_expect_list(test, &ctx->notify.events, names, actions,
				param->accepted ? 1 : 0);
	if (!param->accepted)
		KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, (size_t)0);
}

static struct kunit_case ksmbd_notify_test_cases[] = {
	KUNIT_CASE(ksmbd_notify_test_queue_limit),
	KUNIT_CASE(ksmbd_notify_test_oversized_event),
	KUNIT_CASE_PARAM(ksmbd_notify_test_access, ksmbd_notify_access_gen_params),
	KUNIT_CASE_PARAM(notify_test_name_filter, notify_filter_gen_params),
	KUNIT_CASE(notify_test_filtered_full_queue),
	KUNIT_CASE_PARAM(notify_test_nonrecursive_scope, notify_scope_gen_params),
	{}
};

static struct kunit_suite ksmbd_notify_test_suite = {
	.name = "ksmbd-notify-memory",
	.init = ksmbd_notify_test_init,
	.exit = ksmbd_notify_test_exit,
	.test_cases = ksmbd_notify_test_cases,
};

kunit_test_suite(ksmbd_notify_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for ksmbd notify");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
