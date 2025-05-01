/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2025
 * ChenXiaoSong (chenxiaosong@chenxiaosong.com)
 */
#ifndef __MYDEBUG_H__
#define __MYDEBUG_H__

extern int mydebug_on_types;

#define	MYDEBUG_ON_PRINT	BIT(0)
#define	MYDEBUG_ON_ALL		0xffffffff

#define mydebug_print(fmt, ...)				\
	do {							\
		if (mydebug_on_types & MYDEBUG_ON_PRINT)	\
			pr_info("[%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__);		\
	} while (0)

#define mydebug_print_with_bit(bit, fmt, ...)				\
	do {							\
		if (mydebug_on_types & BIT(bit))	\
			pr_info("[%s:%d] " fmt, __func__, __LINE__, ##__VA_ARGS__);		\
	} while (0)

void mydebug_dump_stack(void);

#endif
