// SPDX-License-Identifier: GPL-2.0-or-later
/* KUnit tests for ksmbd notify. */

#include <kunit/test.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/nls.h>
#include <linux/pseudo_fs.h>
#include <linux/slab.h>
#include <linux/uio.h>

#include "../../common/smb2status.h"
#include "../connection.h"
#include "../ksmbd_work.h"
#include "../notify.h"
#include "../smb_common.h"
#include "../vfs_cache.h"

static int notify_test_tree_init_fs_context(struct fs_context *fc)
{
	return init_pseudo(fc, 0x6b6e6f74) ? 0 : -ENOMEM;
}

static struct file_system_type notify_tree_fs_type = {
	.name = "ksmbd_notify_test",
	.owner = THIS_MODULE,
	.init_fs_context = notify_test_tree_init_fs_context,
	.kill_sb = kill_anon_super,
};

static void notify_test_tree_unmount(void *data)
{
	kern_unmount(data);
}

static void notify_test_tree_dput(void *data)
{
	dput(data);
}

static void notify_test_tree_fput(void *data)
{
	__fput_sync(data);
}

static struct dentry *notify_test_tree_child(struct kunit *test,
					     struct dentry *parent, const char *name,
					     umode_t mode)
{
	struct dentry *dentry;
	struct inode *inode;

	dentry = d_alloc_name(parent, name);
	KUNIT_ASSERT_NOT_NULL(test, dentry);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, notify_test_tree_dput,
							dentry), 0);
	inode = new_inode(parent->d_sb);
	KUNIT_ASSERT_NOT_NULL(test, inode);
	inode->i_ino = iunique(parent->d_sb, 1);
	inode->i_mode = mode | 0700;
	d_add(dentry, inode);
	return dentry;
}

enum notify_tree_node {
	TREE_ROOT, TREE_WATCH, TREE_SUB, TREE_DEEP, TREE_LEAF,
	TREE_SIBLING, TREE_OUTSIDE, TREE_NODE_COUNT,
};

struct notify_tree_case {
	const char *name;
	enum notify_tree_node root;
	enum notify_tree_node event;
	int data_type;
	bool name_event;
	const char *expected;
};

static const struct notify_tree_case notify_tree_cases[] = {
	{ "direct_child", TREE_WATCH, TREE_WATCH, FSNOTIFY_EVENT_DENTRY,
	  true, "leaf" },
	{ "one_level", TREE_WATCH, TREE_SUB, FSNOTIFY_EVENT_DENTRY,
	  true, "sub\\leaf" },
	{ "nested_name", TREE_WATCH, TREE_DEEP, FSNOTIFY_EVENT_DENTRY,
	  true, "sub\\deep\\leaf" },
	{ "nested_dentry", TREE_WATCH, TREE_LEAF, FSNOTIFY_EVENT_DENTRY,
	  false, "sub\\deep\\leaf" },
	{ "nested_path", TREE_WATCH, TREE_LEAF, FSNOTIFY_EVENT_PATH,
	  false, "sub\\deep\\leaf" },
	{ "inode_alias", TREE_WATCH, TREE_LEAF, FSNOTIFY_EVENT_INODE,
	  false, "sub\\deep\\leaf" },
	{ "filesystem_root", TREE_ROOT, TREE_DEEP, FSNOTIFY_EVENT_DENTRY,
	  true, "watch\\sub\\deep\\leaf" },
	{ "root_child", TREE_ROOT, TREE_ROOT, FSNOTIFY_EVENT_DENTRY,
	  true, "leaf" },
	{ "similar_prefix", TREE_WATCH, TREE_SIBLING, FSNOTIFY_EVENT_DENTRY,
	  true, NULL },
	{ "outside_tree", TREE_WATCH, TREE_OUTSIDE, FSNOTIFY_EVENT_PATH,
	  false, NULL },
};

static void notify_test_tree_desc(const struct notify_tree_case *param, char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_test_tree, notify_tree_cases, notify_test_tree_desc);

static void notify_test_tree_relative_name(struct kunit *test)
{
	const struct notify_tree_case *param = test->param_value;
	struct dentry *nodes[TREE_NODE_COUNT];
	struct qstr leaf = QSTR_INIT("leaf", 4), result = {};
	struct ksmbd_file *fp;
	struct ksmbd_notify notify = {};
	struct file *file;
	struct vfsmount *mnt;
	struct path path;
	const void *data;
	char *name;

	mnt = kern_mount(&notify_tree_fs_type);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, mnt);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, notify_test_tree_unmount,
							mnt), 0);
	nodes[TREE_ROOT] = mnt->mnt_root;
	nodes[TREE_WATCH] = notify_test_tree_child(test, mnt->mnt_root, "watch", S_IFDIR);
	nodes[TREE_SUB] = notify_test_tree_child(test, nodes[TREE_WATCH], "sub", S_IFDIR);
	nodes[TREE_DEEP] = notify_test_tree_child(test, nodes[TREE_SUB], "deep", S_IFDIR);
	nodes[TREE_LEAF] = notify_test_tree_child(test, nodes[TREE_DEEP], "leaf", S_IFREG);
	nodes[TREE_SIBLING] = notify_test_tree_child(test, mnt->mnt_root, "watch-other", S_IFDIR);
	nodes[TREE_OUTSIDE] = notify_test_tree_child(test, mnt->mnt_root, "outside", S_IFDIR);
	fp = kunit_kzalloc(test, sizeof(*fp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fp);
	path.mnt = mnt;
	path.dentry = nodes[param->root];
	file = dentry_open(&path, O_PATH, current_cred());
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, file);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, notify_test_tree_fput,
							file), 0);
	fp->filp = file;
	notify.fp = fp;
	path.mnt = mnt;
	path.dentry = nodes[param->event];
	if (param->data_type == FSNOTIFY_EVENT_PATH)
		data = &path;
	else if (param->data_type == FSNOTIFY_EVENT_INODE)
		data = d_inode(path.dentry);
	else
		data = path.dentry;

	name = ksmbd_notify_tree_name_test(&notify, data, param->data_type,
					param->name_event ? d_inode(path.dentry) : NULL,
					param->name_event ? &leaf : NULL, &result);
	if (!param->expected) {
		KUNIT_EXPECT_TRUE(test, IS_ERR(name));
		if (IS_ERR(name))
			KUNIT_EXPECT_EQ(test, PTR_ERR(name), -EXDEV);
		else
			kfree(name);
		return;
	}
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, name);
	KUNIT_EXPECT_PTR_EQ(test, result.name, (const unsigned char *)name);
	KUNIT_EXPECT_EQ(test, result.len, strlen(param->expected));
	KUNIT_EXPECT_STREQ(test, name, param->expected);
	kfree(name);
}

struct ksmbd_notify_test_ctx {
	struct ksmbd_notify notify;
	struct ksmbd_notify_req req;
	struct ksmbd_file fp;
	struct ksmbd_inode ci;
};

static const struct qstr ksmbd_notify_test_name = QSTR_INIT("entry", 5);

/* Tests explicitly dispatch events so timers cannot consume test state. */
static void notify_test_work(struct work_struct *work)
{
}

static int notify_test_init(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	spin_lock_init(&ctx->notify.lock);
	INIT_LIST_HEAD(&ctx->notify.events);
	INIT_DELAYED_WORK(&ctx->notify.broadcast_work, notify_test_work);
	INIT_DELAYED_WORK(&ctx->notify.moved_from_work, notify_test_work);
	ctx->notify.fp = &ctx->fp;
	ctx->notify.filter = FILE_NOTIFY_CHANGE_FILE_NAME;
	ctx->notify.mask = KSMBD_NOTIFY_NAME_EVENT_MASK;
	spin_lock_init(&ctx->fp.f_lock);
	INIT_LIST_HEAD(&ctx->fp.blocked_works);
	ctx->fp.f_ci = &ctx->ci;
	init_rwsem(&ctx->ci.m_lock);
	INIT_LIST_HEAD(&ctx->ci.m_op_list);
	init_waitqueue_head(&ctx->req.wait);
	INIT_LIST_HEAD(&ctx->req.events);
	return 0;
}

static void notify_test_exit(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;

	cancel_delayed_work_sync(&notify->broadcast_work);
	cancel_delayed_work_sync(&notify->moved_from_work);
	ksmbd_notify_free_event_test(notify, notify->moved_from_event);
	ksmbd_notify_free_events_test(notify, &notify->events);
	ksmbd_notify_free_events_test(notify, &ctx->req.events);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
}

static struct ksmbd_notify_event *
notify_test_alloc(struct ksmbd_notify *notify)
{
	return ksmbd_notify_alloc_event_test(notify, FILE_ACTION_ADDED,
					     &ksmbd_notify_test_name, GFP_KERNEL);
}

static void notify_test_fill(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;
	size_t size = kmalloc_size_roundup(sizeof(*event) +
					 ksmbd_notify_test_name.len + 1);

	while (size <= KSMBD_NOTIFY_MAX_BYTES - notify->event_bytes) {
		event = notify_test_alloc(notify);
		KUNIT_ASSERT_NOT_NULL(test, event);
		ksmbd_notify_queue_event_test(notify, event);
	}
}

static void notify_test_queue_limit(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;

	notify_test_fill(test);
	KUNIT_EXPECT_FALSE(test, notify->overflow);
	KUNIT_EXPECT_LE(test, notify->event_bytes,
			(size_t)KSMBD_NOTIFY_MAX_BYTES);
	event = notify_test_alloc(notify);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
	KUNIT_EXPECT_TRUE(test, notify->overflow);
	KUNIT_EXPECT_TRUE(test, list_empty(&notify->events));
	KUNIT_EXPECT_EQ(test, notify->num_events, 0U);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);

	/* No more allocations until a request consumes the overflow. */
	event = notify_test_alloc(notify);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
}

static void notify_test_oversized_event(struct kunit *test)
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

static void notify_test_request_budget(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;
	size_t bytes;

	notify_test_fill(test);
	bytes = notify->event_bytes;
	KUNIT_ASSERT_TRUE(test, ksmbd_notify_take_events_test(notify, &ctx->req));
	KUNIT_EXPECT_TRUE(test, list_empty(&notify->events));
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);

	/* A batch owned by a request still prevents further allocations. */
	event = notify_test_alloc(notify);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
	KUNIT_EXPECT_TRUE(test, notify->overflow);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);

	/* Cancelling that request must neither double-charge nor lose overflow. */
	ksmbd_notify_requeue_events_test(notify, &ctx->req);
	KUNIT_EXPECT_TRUE(test, notify->overflow);
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->req.events));
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
}

static void notify_test_cancel_overflow(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct smb2_change_notify_req req = {};
	struct smb2_change_notify_rsp rsp = {};
	struct ksmbd_notify_event *event;

	ksmbd_notify_set_overflow_test(notify);
	KUNIT_ASSERT_TRUE(test, ksmbd_notify_take_events_test(notify, &ctx->req));
	KUNIT_EXPECT_TRUE(test, ctx->req.overflow);
	KUNIT_EXPECT_FALSE(test, notify->overflow);

	event = notify_test_alloc(notify);
	KUNIT_ASSERT_NOT_NULL(test, event);
	ksmbd_notify_queue_event_test(notify, event);
	ksmbd_notify_requeue_events_test(notify, &ctx->req);
	KUNIT_EXPECT_TRUE(test, notify->overflow);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);

	KUNIT_ASSERT_TRUE(test, ksmbd_notify_take_events_test(notify, &ctx->req));
	/* Overflow replies must not encode records or access work->conn. */
	KUNIT_EXPECT_EQ(test, ksmbd_notify_reply_test(NULL, &req, &rsp, notify,
						      &ctx->req), 0);
	KUNIT_EXPECT_EQ(test, rsp.hdr.Status, STATUS_NOTIFY_ENUM_DIR);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(rsp.OutputBufferLength), 0U);

	event = notify_test_alloc(notify);
	KUNIT_ASSERT_NOT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
}

static void notify_test_requeue_budget(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;
	size_t bytes;

	notify_test_fill(test);
	bytes = notify->event_bytes;
	KUNIT_ASSERT_TRUE(test, ksmbd_notify_take_events_test(notify, &ctx->req));
	ksmbd_notify_requeue_events_test(notify, &ctx->req);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	KUNIT_EXPECT_FALSE(test, notify->overflow);
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->req.events));
	KUNIT_EXPECT_GT(test, notify->num_events, 0U);
	event = notify_test_alloc(notify);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
	KUNIT_EXPECT_TRUE(test, notify->overflow);
}

static void notify_test_wake_overflow(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	void *argv[] = { &ctx->req };
	struct ksmbd_work work = {
		.state = KSMBD_WORK_ACTIVE,
		.cancel_argv = argv,
	};

	ksmbd_notify_set_cancel_fn_test(&work);
	list_add_tail(&work.fp_entry, &ctx->fp.blocked_works);
	ksmbd_notify_set_overflow_test(&ctx->notify);
	ksmbd_notify_broadcast_test(&ctx->notify);
	list_del_init(&work.fp_entry);
	KUNIT_EXPECT_TRUE(test, ctx->req.notified);
	KUNIT_EXPECT_TRUE(test, ctx->req.overflow);
	KUNIT_EXPECT_EQ(test, ctx->req.num_events, 0U);
	KUNIT_EXPECT_FALSE(test, ctx->notify.overflow);
}

static void notify_test_rename_limit(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;

	ksmbd_notify_save_moved_from_test(notify, FS_MOVED_FROM, 1,
					  &ksmbd_notify_test_name);
	KUNIT_ASSERT_NOT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_GT(test, notify->event_bytes, (size_t)0);
	notify_test_fill(test);
	KUNIT_EXPECT_TRUE(test, ksmbd_notify_handle_rename_test(notify, FS_MOVED_TO,
								1, &ksmbd_notify_test_name));
	KUNIT_EXPECT_TRUE(test, notify->overflow);
	KUNIT_EXPECT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
	ksmbd_notify_moved_from_timeout_test(&notify->moved_from_work.work);
	KUNIT_EXPECT_TRUE(test, list_empty(&notify->events));

	KUNIT_ASSERT_TRUE(test, ksmbd_notify_take_events_test(notify, &ctx->req));
	ksmbd_notify_save_moved_from_test(notify, FS_MOVED_FROM, 2,
					  &ksmbd_notify_test_name);
	KUNIT_EXPECT_TRUE(test, ksmbd_notify_handle_rename_test(notify, FS_MOVED_TO,
								2, &ksmbd_notify_test_name));
	KUNIT_ASSERT_EQ(test, notify->num_events, 2U);
	event = list_first_entry(&notify->events, struct ksmbd_notify_event, list);
	KUNIT_EXPECT_EQ(test, event->action, (u32)FILE_ACTION_RENAMED_OLD_NAME);
	event = list_next_entry(event, list);
	KUNIT_EXPECT_EQ(test, event->action, (u32)FILE_ACTION_RENAMED_NEW_NAME);
}

static void notify_test_filtered_rename(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;

	notify->filter = 0;
	notify->mask = 0;
	ksmbd_notify_handle_inode_event_test(notify, FS_MOVED_FROM, NULL, NULL,
					     &ksmbd_notify_test_name, 1);
	KUNIT_EXPECT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
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

static void notify_test_access_desc(const struct ksmbd_notify_access_case *param,
				    char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_test_access, ksmbd_notify_access_cases,
		  notify_test_access_desc);

static void notify_test_access(struct kunit *test)
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

static void notify_test_filter_desc(const struct notify_filter_case *param, char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_test_filter, notify_filter_cases, notify_test_filter_desc);

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

	notify_test_fill(test);
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

static void notify_test_scope_desc(const struct notify_scope_case *param, char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_test_scope, notify_scope_cases, notify_test_scope_desc);

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
static void notify_test_inode_event(struct kunit *test, u32 mask,
				    const char *name, u32 cookie)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct qstr qname = QSTR_INIT(name, strlen(name));

	KUNIT_ASSERT_EQ(test, ksmbd_notify_handle_inode_event_test(&ctx->notify,
				mask, NULL, NULL, &qname, cookie), 0);
}

static void notify_test_rename_timeout(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	static const char * const names[] = { "old" };
	const u32 actions[] = { FILE_ACTION_REMOVED };
	size_t bytes;

	notify_test_inode_event(test, FS_MOVED_FROM, "old", 7);
	KUNIT_ASSERT_NOT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_TRUE(test, list_empty(&notify->events));
	KUNIT_EXPECT_EQ(test, notify->num_events, 0U);
	bytes = notify->event_bytes;
	KUNIT_EXPECT_EQ(test, bytes, notify->moved_from_event->alloc_size);
	cancel_delayed_work_sync(&notify->moved_from_work);
	ksmbd_notify_moved_from_timeout_test(&notify->moved_from_work.work);
	KUNIT_EXPECT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_EQ(test, notify->num_events, 1U);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	notify_test_expect_list(test, &notify->events, names, actions, 1);

	/* A second timeout must not report the same removal twice. */
	ksmbd_notify_moved_from_timeout_test(&notify->moved_from_work.work);
	KUNIT_EXPECT_EQ(test, notify->num_events, 1U);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	notify_test_expect_list(test, &notify->events, names, actions, 1);
}

static void notify_test_rename_source_replaced(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	static const char * const names[] = { "first", "second" };
	const u32 actions[] = { FILE_ACTION_REMOVED, FILE_ACTION_REMOVED };
	size_t first_bytes, bytes;

	notify_test_inode_event(test, FS_MOVED_FROM, "first", 1);
	KUNIT_ASSERT_NOT_NULL(test, notify->moved_from_event);
	first_bytes = notify->event_bytes;
	notify_test_inode_event(test, FS_MOVED_FROM, "second", 2);
	KUNIT_ASSERT_NOT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_STREQ(test, notify->moved_from_event->name, "second");
	KUNIT_EXPECT_EQ(test, notify->moved_from_cookie, 2U);
	KUNIT_EXPECT_EQ(test, notify->num_events, 1U);
	notify_test_expect_list(test, &notify->events, names, actions, 1);
	bytes = first_bytes + notify->moved_from_event->alloc_size;
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	cancel_delayed_work_sync(&notify->moved_from_work);
	ksmbd_notify_moved_from_timeout_test(&notify->moved_from_work.work);
	KUNIT_EXPECT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_EQ(test, notify->num_events, 2U);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	notify_test_expect_list(test, &notify->events, names, actions, 2);
}
static void notify_test_rename_names(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	static const char * const names[] = { "old-name", "new-name" };
	const u32 actions[] = { FILE_ACTION_RENAMED_OLD_NAME,
				FILE_ACTION_RENAMED_NEW_NAME };
	struct ksmbd_notify_event *old;
	size_t bytes;

	notify_test_inode_event(test, FS_MOVED_FROM, names[0], 42);
	old = notify->moved_from_event;
	KUNIT_ASSERT_NOT_NULL(test, old);
	notify_test_inode_event(test, FS_MOVED_TO, names[1], 42);
	KUNIT_EXPECT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_EQ(test, notify->moved_from_mask, 0U);
	KUNIT_EXPECT_EQ(test, notify->moved_from_cookie, 0U);
	KUNIT_EXPECT_EQ(test, notify->num_events, 2U);
	notify_test_expect_list(test, &notify->events, names, actions, 2);
	KUNIT_EXPECT_PTR_EQ(test, list_first_entry(&notify->events,
				struct ksmbd_notify_event, list), old);
	bytes = notify->event_bytes;
	ksmbd_notify_moved_from_timeout_test(&notify->moved_from_work.work);
	KUNIT_EXPECT_EQ(test, notify->num_events, 2U);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	notify_test_expect_list(test, &notify->events, names, actions, 2);
}

static void notify_test_rename_without_source(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	static const char * const names[] = { "incoming" };
	const u32 actions[] = { FILE_ACTION_ADDED };
	struct ksmbd_notify_event *event;

	notify_test_inode_event(test, FS_MOVED_TO, names[0], 42);
	KUNIT_EXPECT_NULL(test, ctx->notify.moved_from_event);
	KUNIT_EXPECT_EQ(test, ctx->notify.num_events, 1U);
	notify_test_expect_list(test, &ctx->notify.events, names, actions, 1);
	KUNIT_ASSERT_FALSE(test, list_empty(&ctx->notify.events));
	event = list_first_entry(&ctx->notify.events, struct ksmbd_notify_event, list);
	KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, event->alloc_size);
}

static void notify_test_rename_cookie_mismatch(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	static const char * const names[] = { "incoming", "outgoing" };
	const u32 actions[] = { FILE_ACTION_ADDED, FILE_ACTION_REMOVED };
	struct ksmbd_notify_event *old, *added;
	size_t bytes;

	notify_test_inode_event(test, FS_MOVED_FROM, "outgoing", 1);
	old = notify->moved_from_event;
	KUNIT_ASSERT_NOT_NULL(test, old);
	notify_test_inode_event(test, FS_MOVED_TO, "incoming", 2);
	KUNIT_EXPECT_PTR_EQ(test, notify->moved_from_event, old);
	KUNIT_EXPECT_EQ(test, notify->moved_from_cookie, 1U);
	KUNIT_EXPECT_EQ(test, notify->num_events, 1U);
	notify_test_expect_list(test, &notify->events, names, actions, 1);
	KUNIT_ASSERT_FALSE(test, list_empty(&notify->events));
	added = list_first_entry(&notify->events, struct ksmbd_notify_event, list);
	bytes = old->alloc_size + added->alloc_size;
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	cancel_delayed_work_sync(&notify->moved_from_work);
	ksmbd_notify_moved_from_timeout_test(&notify->moved_from_work.work);
	KUNIT_EXPECT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_EQ(test, notify->num_events, 2U);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	notify_test_expect_list(test, &notify->events, names, actions, 2);
}
static struct ksmbd_notify_event *
notify_test_add(struct kunit *test, u32 action, const char *name)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct qstr qname = QSTR_INIT(name, strlen(name));
	struct ksmbd_notify_event *event;

	event = ksmbd_notify_alloc_event_test(&ctx->notify, action, &qname,
					    GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, event);
	ksmbd_notify_queue_event_test(&ctx->notify, event);
	return event;
}
static void notify_test_unload_nls(void *data)
{
	unload_nls(data);
}

static void notify_test_kvfree(void *data)
{
	kvfree(data);
}

static void *notify_test_encode(struct kunit *test, u32 max_len, size_t *len)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_work *work;
	struct ksmbd_conn *conn;
	void *data;

	work = kunit_kzalloc(test, sizeof(*work), GFP_KERNEL);
	conn = kunit_kzalloc(test, sizeof(*conn), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, work);
	KUNIT_ASSERT_NOT_NULL(test, conn);
	conn->local_nls = load_nls("utf8");
	KUNIT_ASSERT_NOT_NULL(test, conn->local_nls);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, notify_test_unload_nls,
						      conn->local_nls), 0);
	work->conn = conn;
	data = ksmbd_notify_encode_events_test(work, &ctx->notify.events, max_len, len);
	if (data)
		KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, notify_test_kvfree,
							      data), 0);
	return data;
}

static void notify_test_expect_record(struct kunit *test, const u8 *data,
				     size_t len, size_t offset, u32 action,
				     const u8 *name, u32 name_len, u32 next)
{
	const struct file_notify_information *info;

	KUNIT_ASSERT_LE(test, offset + sizeof(*info) + name_len, len);
	info = (const struct file_notify_information *)(data + offset);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(info->NextEntryOffset), next);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(info->Action), action);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(info->FileNameLength), name_len);
	KUNIT_EXPECT_MEMEQ(test, info->FileName, name, name_len);
}

static void notify_test_encode_padding(struct kunit *test)
{
	/* 20 protocol bytes, followed by four zeroed compound-padding bytes. */
	const u8 expected[] = {
		0, 0, 0, 0, 1, 0, 0, 0, 6, 0, 0, 0,
		'a', 0, 'b', 0, 'c', 0, 0, 0, 0, 0, 0, 0,
	};
	size_t len = 0;
	u8 *data;

	notify_test_add(test, FILE_ACTION_ADDED, "abc");
	data = notify_test_encode(test, 20, &len);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_EXPECT_EQ(test, len, (size_t)20);
	KUNIT_EXPECT_MEMEQ(test, data, expected, sizeof(expected));
}

static void notify_test_encode_unicode_order(struct kunit *test)
{
	const u8 ascii[] = { 'a', 0 };
	const u8 chinese[] = { 0x2d, 0x4e };
	const u8 emoji[] = { 0x3d, 0xd8, 0x00, 0xde };
	struct ksmbd_notify_event *event;
	size_t len = 0;
	u8 *data;

	/* Insert out of order; equal timestamps must keep insertion order. */
	event = notify_test_add(test, FILE_ACTION_REMOVED, "\xf0\x9f\x98\x80");
	event->when = 20;
	event = notify_test_add(test, FILE_ACTION_ADDED, "a");
	event->when = 10;
	event = notify_test_add(test, FILE_ACTION_MODIFIED, "\xe4\xb8\xad");
	event->when = 10;
	data = notify_test_encode(test, 48, &len);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_EXPECT_EQ(test, len, (size_t)48);
	notify_test_expect_record(test, data, len, 0, FILE_ACTION_ADDED,
				  ascii, sizeof(ascii), 16);
	notify_test_expect_record(test, data, len, 16, FILE_ACTION_MODIFIED,
				  chinese, sizeof(chinese), 16);
	notify_test_expect_record(test, data, len, 32, FILE_ACTION_REMOVED,
				  emoji, sizeof(emoji), 0);
	KUNIT_EXPECT_EQ(test, data[14], 0);
	KUNIT_EXPECT_EQ(test, data[15], 0);
	KUNIT_EXPECT_EQ(test, data[30], 0);
	KUNIT_EXPECT_EQ(test, data[31], 0);
}

struct notify_encode_limit_case {
	const char *name;
	u32 limit;
	bool fits;
};

static const struct notify_encode_limit_case notify_encode_limit_cases[] = {
	{ "zero", 0, false },
	{ "one_byte_short", 19, false },
	{ "exact", 20, true },
	{ "one_byte_extra", 21, true },
};

static void notify_test_encode_limit_desc(const struct notify_encode_limit_case *param,
					  char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_test_encode_limit, notify_encode_limit_cases,
		  notify_test_encode_limit_desc);

static void notify_test_encode_limit(struct kunit *test)
{
	const struct notify_encode_limit_case *param = test->param_value;
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	size_t len = 123, bytes;
	void *data;

	notify_test_add(test, FILE_ACTION_ADDED, "abc");
	bytes = ctx->notify.event_bytes;
	data = notify_test_encode(test, param->limit, &len);
	if (param->fits) {
		KUNIT_ASSERT_NOT_NULL(test, data);
		KUNIT_EXPECT_EQ(test, len, (size_t)20);
	} else {
		KUNIT_EXPECT_NULL(test, data);
		KUNIT_EXPECT_EQ(test, len, (size_t)0);
	}
	/* Encoding neither owns nor releases the original event records. */
	KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, bytes);
	KUNIT_EXPECT_EQ(test, ctx->notify.num_events, 1U);
}

static void notify_test_encode_batch_too_large(struct kunit *test)
{
	size_t len = 123;
	void *data;

	notify_test_add(test, FILE_ACTION_ADDED, "a");
	notify_test_add(test, FILE_ACTION_ADDED, "b");
	data = notify_test_encode(test, 31, &len);
	KUNIT_EXPECT_NULL(test, data);
	KUNIT_EXPECT_EQ(test, len, (size_t)0);
}

struct notify_duplicate_case {
	const char *name;
	const char *names[3];
	u32 actions[3];
	unsigned int count;
};

static const struct notify_duplicate_case notify_duplicate_cases[] = {
	{ "adjacent", { "same", "same", "same" },
	  { FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED }, 1 },
	{ "case_sensitive", { "Same", "same", "same" },
	  { FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED }, 2 },
	{ "different_action", { "same", "same", "same" },
	  { FILE_ACTION_ADDED, FILE_ACTION_REMOVED, FILE_ACTION_MODIFIED }, 3 },
	{ "different_length", { "file", "file2", "file2" },
	  { FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED }, 2 },
	{ "nonadjacent", { "a", "b", "a" },
	  { FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED, FILE_ACTION_MODIFIED }, 3 },
};

static void notify_test_duplicate_desc(const struct notify_duplicate_case *param,
				       char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_test_duplicate, notify_duplicate_cases, notify_test_duplicate_desc);

static void notify_test_encode_duplicates(struct kunit *test)
{
	const struct notify_duplicate_case *param = test->param_value;
	struct ksmbd_notify_event *event;
	size_t len = 0, offset = 0;
	unsigned int i, j;
	u8 *data;

	for (i = 0; i < 3; i++) {
		event = notify_test_add(test, param->actions[i], param->names[i]);
		event->when = i + 1;
	}
	data = notify_test_encode(test, 128, &len);
	KUNIT_ASSERT_NOT_NULL(test, data);
	for (i = 0; i < param->count; i++) {
		const struct file_notify_information *info;
		u8 expected[16] = {};
		size_t name_len = strlen(param->names[i]);
		u32 next;

		KUNIT_ASSERT_LE(test, offset + sizeof(*info), len);
		info = (const struct file_notify_information *)(data + offset);
		for (j = 0; j < name_len; j++)
			expected[j * 2] = param->names[i][j];
		next = i + 1 == param->count ? 0 : ALIGN(12 + name_len * 2, 4);
		notify_test_expect_record(test, data, len, offset, param->actions[i],
					  expected, name_len * 2, next);
		offset += ALIGN(12 + name_len * 2, 4);
	}
	KUNIT_EXPECT_EQ(test, offset, len);
}
struct notify_test_waiter {
	struct ksmbd_work work;
	struct ksmbd_notify_req req;
	struct ksmbd_notify *notify;
	void *arg;
};

static void notify_test_waiter_free(void *data)
{
	struct notify_test_waiter *waiter = data;

	list_del_init(&waiter->work.fp_entry);
	ksmbd_notify_free_events_test(waiter->notify, &waiter->req.events);
}

static struct notify_test_waiter *notify_test_waiter_add(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct notify_test_waiter *waiter;

	waiter = kunit_kzalloc(test, sizeof(*waiter), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, waiter);
	waiter->notify = &ctx->notify;
	init_waitqueue_head(&waiter->req.wait);
	INIT_LIST_HEAD(&waiter->req.events);
	INIT_LIST_HEAD(&waiter->work.fp_entry);
	waiter->work.state = KSMBD_WORK_ACTIVE;
	waiter->arg = &waiter->req;
	waiter->work.cancel_argv = &waiter->arg;
	ksmbd_notify_set_cancel_fn_test(&waiter->work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, notify_test_waiter_free,
						      waiter), 0);
	list_add_tail(&waiter->work.fp_entry, &ctx->fp.blocked_works);
	return waiter;
}

static void notify_test_waiter_fifo(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct notify_test_waiter *first, *second;
	static const char * const pair[] = { "before", "after" };
	const u32 actions[] = { FILE_ACTION_RENAMED_OLD_NAME,
				FILE_ACTION_RENAMED_NEW_NAME };
	static const char * const later[] = { "later" };
	const u32 added[] = { FILE_ACTION_ADDED };
	size_t bytes;

	first = notify_test_waiter_add(test);
	second = notify_test_waiter_add(test);
	notify_test_inode_event(test, FS_MOVED_FROM, pair[0], 11);
	notify_test_inode_event(test, FS_MOVED_TO, pair[1], 11);
	bytes = ctx->notify.event_bytes;
	ksmbd_notify_broadcast_test(&ctx->notify);
	KUNIT_EXPECT_TRUE(test, first->req.notified);
	KUNIT_EXPECT_FALSE(test, second->req.notified);
	KUNIT_EXPECT_EQ(test, first->req.num_events, 2U);
	KUNIT_EXPECT_TRUE(test, list_empty(&second->req.events));
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->notify.events));
	KUNIT_EXPECT_EQ(test, ctx->notify.num_events, 0U);
	KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, bytes);
	notify_test_expect_list(test, &first->req.events, pair, actions, 2);
	ksmbd_notify_broadcast_test(&ctx->notify);
	KUNIT_EXPECT_FALSE(test, second->req.notified);

	/* The first request stays linked, but must not take another batch. */
	notify_test_add(test, FILE_ACTION_ADDED, "later");
	ksmbd_notify_broadcast_test(&ctx->notify);
	KUNIT_EXPECT_TRUE(test, second->req.notified);
	KUNIT_EXPECT_EQ(test, second->req.num_events, 1U);
	notify_test_expect_list(test, &first->req.events, pair, actions, 2);
	notify_test_expect_list(test, &second->req.events, later, added, 1);
	kunit_release_action(test, notify_test_waiter_free, first);
	kunit_release_action(test, notify_test_waiter_free, second);
	KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, (size_t)0);
}

enum notify_waiter_skip {
	WAITER_CANCELLED, WAITER_CLOSED, WAITER_NOTIFIED,
	WAITER_OTHER_CALLBACK, WAITER_NO_ARGS,
};

struct notify_waiter_case {
	const char *name;
	enum notify_waiter_skip reason;
};

static const struct notify_waiter_case notify_waiter_cases[] = {
	{ "cancelled", WAITER_CANCELLED },
	{ "closed", WAITER_CLOSED },
	{ "already_notified", WAITER_NOTIFIED },
	{ "other_callback", WAITER_OTHER_CALLBACK },
	{ "missing_arguments", WAITER_NO_ARGS },
};

static void notify_test_waiter_desc(const struct notify_waiter_case *param, char *desc)
{
	strscpy(desc, param->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(notify_test_waiter, notify_waiter_cases, notify_test_waiter_desc);

static void notify_test_other_cancel(void **argv)
{
}

static void notify_test_waiter_skip(struct kunit *test)
{
	const struct notify_waiter_case *param = test->param_value;
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct notify_test_waiter *skip, *active;
	static const char * const names[] = { "entry" };
	const u32 actions[] = { FILE_ACTION_ADDED };

	skip = notify_test_waiter_add(test);
	active = notify_test_waiter_add(test);
	switch (param->reason) {
	case WAITER_CANCELLED:
		skip->work.state = KSMBD_WORK_CANCELLED;
		break;
	case WAITER_CLOSED:
		skip->work.state = KSMBD_WORK_CLOSED;
		break;
	case WAITER_NOTIFIED:
		skip->req.notified = true;
		break;
	case WAITER_OTHER_CALLBACK:
		skip->work.cancel_fn = notify_test_other_cancel;
		break;
	case WAITER_NO_ARGS:
		skip->work.cancel_argv = NULL;
		break;
	}
	notify_test_add(test, FILE_ACTION_ADDED, "entry");
	ksmbd_notify_broadcast_test(&ctx->notify);
	KUNIT_EXPECT_EQ(test, skip->req.notified, param->reason == WAITER_NOTIFIED);
	KUNIT_EXPECT_EQ(test, skip->req.num_events, 0U);
	KUNIT_EXPECT_TRUE(test, list_empty(&skip->req.events));
	KUNIT_EXPECT_TRUE(test, active->req.notified);
	KUNIT_EXPECT_EQ(test, active->req.num_events, 1U);
	notify_test_expect_list(test, &active->req.events, names, actions, 1);
	kunit_release_action(test, notify_test_waiter_free, active);

	/* With only an ineligible waiter left, synchronous delivery is allowed. */
	notify_test_add(test, FILE_ACTION_ADDED, "entry");
	KUNIT_EXPECT_TRUE(test, ksmbd_notify_take_events_test(&ctx->notify, &ctx->req));
	KUNIT_EXPECT_EQ(test, ctx->req.num_events, 1U);
	notify_test_expect_list(test, &ctx->req.events, names, actions, 1);
	KUNIT_EXPECT_TRUE(test, list_empty(&skip->req.events));
	kunit_release_action(test, notify_test_waiter_free, skip);
}

static void notify_test_waiter_before_sync(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct notify_test_waiter *waiter = notify_test_waiter_add(test);
	static const char * const names[] = { "queued" };
	const u32 actions[] = { FILE_ACTION_ADDED };
	size_t bytes;

	notify_test_add(test, FILE_ACTION_ADDED, "queued");
	bytes = ctx->notify.event_bytes;
	KUNIT_EXPECT_FALSE(test, ksmbd_notify_take_events_test(&ctx->notify, &ctx->req));
	KUNIT_EXPECT_TRUE(test, waiter->req.notified);
	KUNIT_EXPECT_EQ(test, waiter->req.num_events, 1U);
	KUNIT_EXPECT_EQ(test, ctx->req.num_events, 0U);
	KUNIT_EXPECT_FALSE(test, ctx->req.overflow);
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->req.events));
	KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, bytes);
	notify_test_expect_list(test, &waiter->req.events, names, actions, 1);
	kunit_release_action(test, notify_test_waiter_free, waiter);
}

static void notify_test_waiter_empty(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct notify_test_waiter *waiter = notify_test_waiter_add(test);

	ksmbd_notify_broadcast_test(&ctx->notify);
	KUNIT_EXPECT_FALSE(test, ksmbd_notify_take_events_test(&ctx->notify, &ctx->req));
	KUNIT_EXPECT_FALSE(test, waiter->req.notified);
	KUNIT_EXPECT_TRUE(test, list_empty(&waiter->req.events));
	KUNIT_EXPECT_EQ(test, waiter->req.num_events, 0U);
	KUNIT_EXPECT_EQ(test, ctx->notify.event_bytes, (size_t)0);
	kunit_release_action(test, notify_test_waiter_free, waiter);
}

static struct kunit_case ksmbd_notify_test_cases[] = {
	KUNIT_CASE_PARAM(notify_test_tree_relative_name, notify_test_tree_gen_params),
	KUNIT_CASE(notify_test_queue_limit),
	KUNIT_CASE(notify_test_oversized_event),
	KUNIT_CASE(notify_test_request_budget),
	KUNIT_CASE(notify_test_cancel_overflow),
	KUNIT_CASE(notify_test_requeue_budget),
	KUNIT_CASE(notify_test_wake_overflow),
	KUNIT_CASE(notify_test_rename_limit),
	KUNIT_CASE(notify_test_filtered_rename),
	KUNIT_CASE_PARAM(notify_test_access, notify_test_access_gen_params),
	KUNIT_CASE_PARAM(notify_test_name_filter, notify_test_filter_gen_params),
	KUNIT_CASE(notify_test_filtered_full_queue),
	KUNIT_CASE_PARAM(notify_test_nonrecursive_scope, notify_test_scope_gen_params),
	KUNIT_CASE(notify_test_rename_timeout),
	KUNIT_CASE(notify_test_rename_source_replaced),
	KUNIT_CASE(notify_test_rename_names),
	KUNIT_CASE(notify_test_rename_without_source),
	KUNIT_CASE(notify_test_rename_cookie_mismatch),
	KUNIT_CASE(notify_test_encode_padding),
	KUNIT_CASE(notify_test_encode_unicode_order),
	KUNIT_CASE_PARAM(notify_test_encode_limit, notify_test_encode_limit_gen_params),
	KUNIT_CASE(notify_test_encode_batch_too_large),
	KUNIT_CASE_PARAM(notify_test_encode_duplicates, notify_test_duplicate_gen_params),
	KUNIT_CASE(notify_test_waiter_fifo),
	KUNIT_CASE_PARAM(notify_test_waiter_skip, notify_test_waiter_gen_params),
	KUNIT_CASE(notify_test_waiter_before_sync),
	KUNIT_CASE(notify_test_waiter_empty),
	{}
};

static struct kunit_suite ksmbd_notify_test_suite = {
	.name = "ksmbd-notify",
	.init = notify_test_init,
	.exit = notify_test_exit,
	.test_cases = ksmbd_notify_test_cases,
};

kunit_test_suite(ksmbd_notify_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for ksmbd notify");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
