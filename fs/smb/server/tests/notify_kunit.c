// SPDX-License-Identifier: GPL-2.0-or-later
/* KUnit tests for ksmbd notify. */

#include <kunit/test.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/mount.h>
#include <linux/pseudo_fs.h>

#include "../notify.h"
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

static struct kunit_case ksmbd_notify_test_cases[] = {
	KUNIT_CASE_PARAM(notify_test_tree_relative_name, notify_test_tree_gen_params),
	{}
};

static struct kunit_suite ksmbd_notify_test_suite = {
	.name = "ksmbd-notify",
	.test_cases = ksmbd_notify_test_cases,
};

kunit_test_suite(ksmbd_notify_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for ksmbd notify");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
