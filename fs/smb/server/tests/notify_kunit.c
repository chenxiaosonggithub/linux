// SPDX-License-Identifier: GPL-2.0-or-later
/* KUnit tests for ksmbd notify. */

#include <kunit/test.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>
#include <linux/slab.h>
#include <linux/uio.h>

#include "../../common/smb2status.h"
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
	struct ksmbd_file fp;
};

static const struct qstr ksmbd_notify_test_name = QSTR_INIT("entry", 5);

static int notify_test_init(struct kunit *test)
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

static void notify_test_exit(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;

	ksmbd_notify_free_events_test(notify, &notify->events);
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

static struct kunit_case ksmbd_notify_test_cases[] = {
	KUNIT_CASE_PARAM(notify_test_tree_relative_name, notify_test_tree_gen_params),
	KUNIT_CASE(notify_test_queue_limit),
	KUNIT_CASE(notify_test_oversized_event),
	KUNIT_CASE_PARAM(notify_test_access, notify_test_access_gen_params),
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
