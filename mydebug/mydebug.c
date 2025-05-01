// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025
 * ChenXiaoSong (chenxiaosong@chenxiaosong.com)
 *
 *  from
 *
 *  fs/smb/server/server.c
 *
 *  Copyright (C) 2016 Namjae Jeon <linkinjeon@kernel.org>
 *  Copyright (C) 2018 Samsung Electronics Co., Ltd.
 */

#include <linux/module.h>
#include <linux/device/class.h>
#include <linux/stacktrace.h>
#include <mydebug.h>

int mydebug_on_types = MYDEBUG_ON_PRINT;
EXPORT_SYMBOL(mydebug_on_types);

static ssize_t debug_show(const struct class *class,
	const struct class_attribute *attr, char *buf)
{
	ssize_t sz = 0;
	int i, pos = 0;

	for (i = 0; i < sizeof(mydebug_on_types) * 8; i++) {
		if ((mydebug_on_types >> i) & 1) {
			pos = sysfs_emit_at(buf, sz, "[%d] ", i);
		}
		sz += pos;
	}
	sz += sysfs_emit_at(buf, sz, "\n");
	return sz;
}

static ssize_t debug_store(const struct class *class, const struct class_attribute *attr,
			   const char *buf, size_t len)
{
	int i;
	char str[4];

	for (i = 0; i < sizeof(mydebug_on_types) * 8; i++) {
		sprintf(str, "%d", i);
		if (sysfs_streq(buf, "all")) {
			if (mydebug_on_types == MYDEBUG_ON_ALL)
				mydebug_on_types = 0;
			else
				mydebug_on_types = MYDEBUG_ON_ALL;
			break;
		}

		if (sysfs_streq(buf, str)) {
			if (mydebug_on_types & (1 << i))
				mydebug_on_types &= ~(1 << i);
			else
				mydebug_on_types |= (1 << i);
			break;
		}
	}

	return len;
}

static CLASS_ATTR_RW(debug);

static struct attribute *mydebug_ctrl_class_attrs[] = {
	&class_attr_debug.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mydebug_ctrl_class);

struct class mydebug_ctrl_class = {
	.name		= "mydebug-ctrl",
	.class_groups	= mydebug_ctrl_class_groups,
};

#define MAX_LINES_PER_STACK	64 // 每个栈最大的行数
static unsigned long stack_lines[MAX_LINES_PER_STACK];

static void mydebug_stack_trace_print(const unsigned long *entries,
				      unsigned int nr_entries,
				      int spaces)
{
	unsigned int i;

	if (WARN_ON(!entries))
		return;

	for (i = 0; i < nr_entries; i++)
		trace_printk("%*c%pS\n", 1 + spaces, ' ', (void *)entries[i]);
}

void mydebug_dump_stack(void)
{
	unsigned int trace_len;
	trace_len = stack_trace_save(stack_lines, MAX_LINES_PER_STACK, 0);
	trace_printk("comm:%s, pid:%d\n", current->comm, current->pid);
	mydebug_stack_trace_print(stack_lines, trace_len, 0);
	trace_printk("\n");
}
EXPORT_SYMBOL(mydebug_dump_stack);

static int __init init_mydebug(void)
{
	int ret = 0;

	ret = class_register(&mydebug_ctrl_class);
	if (ret) {
		pr_err("Unable to register mydebug-ctrl class\n");
		return ret;
	}

	mydebug_print("success\n");
	return 0;
}

static void __exit exit_mydebug(void)
{
	mydebug_print("\n");
	class_unregister(&mydebug_ctrl_class);
}

MODULE_AUTHOR("ChenXiaoSong");
MODULE_DESCRIPTION("My Debug");
MODULE_LICENSE("GPL");
module_init(init_mydebug)
module_exit(exit_mydebug)
