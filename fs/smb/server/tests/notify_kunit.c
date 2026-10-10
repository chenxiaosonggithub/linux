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
	struct ksmbd_notify_req req;
	struct ksmbd_file fp;
	struct ksmbd_inode ci;
};

static const struct qstr ksmbd_notify_test_name = QSTR_INIT("entry", 5);

/* Tests explicitly dispatch events so timers cannot consume test state. */
static void ksmbd_notify_test_work(struct work_struct *work)
{
}

static int ksmbd_notify_test_init(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	spin_lock_init(&ctx->notify.lock);
	INIT_LIST_HEAD(&ctx->notify.events);
	INIT_DELAYED_WORK(&ctx->notify.broadcast_work, ksmbd_notify_test_work);
	INIT_DELAYED_WORK(&ctx->notify.moved_from_work, ksmbd_notify_test_work);
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

static void ksmbd_notify_test_exit(struct kunit *test)
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

static void ksmbd_notify_test_request_budget(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;
	size_t bytes;

	ksmbd_notify_test_fill(test);
	bytes = notify->event_bytes;
	KUNIT_ASSERT_TRUE(test, ksmbd_notify_take_events_test(notify, &ctx->req));
	KUNIT_EXPECT_TRUE(test, list_empty(&notify->events));
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);

	/* A batch owned by a request still prevents further allocations. */
	event = ksmbd_notify_test_alloc(notify);
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

static void ksmbd_notify_test_cancel_overflow(struct kunit *test)
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

	event = ksmbd_notify_test_alloc(notify);
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
	KUNIT_EXPECT_FALSE(test, ksmbd_notify_events_pending_test(notify));

	event = ksmbd_notify_test_alloc(notify);
	KUNIT_ASSERT_NOT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
}

static void ksmbd_notify_test_requeue_budget(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;
	size_t bytes;

	ksmbd_notify_test_fill(test);
	bytes = notify->event_bytes;
	KUNIT_ASSERT_TRUE(test, ksmbd_notify_take_events_test(notify, &ctx->req));
	ksmbd_notify_requeue_events_test(notify, &ctx->req);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, bytes);
	KUNIT_EXPECT_FALSE(test, notify->overflow);
	KUNIT_EXPECT_TRUE(test, list_empty(&ctx->req.events));
	KUNIT_EXPECT_GT(test, notify->num_events, 0U);
	event = ksmbd_notify_test_alloc(notify);
	KUNIT_EXPECT_NULL(test, event);
	ksmbd_notify_free_event_test(notify, event);
	KUNIT_EXPECT_TRUE(test, notify->overflow);
}

static void ksmbd_notify_test_wake_overflow(struct kunit *test)
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

static void ksmbd_notify_test_rename_limit(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;
	struct ksmbd_notify_event *event;

	ksmbd_notify_save_moved_from_test(notify, FS_MOVED_FROM, 1,
					  &ksmbd_notify_test_name);
	KUNIT_ASSERT_NOT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_GT(test, notify->event_bytes, (size_t)0);
	ksmbd_notify_test_fill(test);
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

static void ksmbd_notify_test_filtered_rename(struct kunit *test)
{
	struct ksmbd_notify_test_ctx *ctx = test->priv;
	struct ksmbd_notify *notify = &ctx->notify;

	notify->filter = 0;
	notify->mask = 0;
	ksmbd_notify_handle_inode_event_test(notify, FS_MOVED_FROM, NULL, NULL,
					     &ksmbd_notify_test_name, 1);
	KUNIT_EXPECT_NULL(test, notify->moved_from_event);
	KUNIT_EXPECT_EQ(test, notify->event_bytes, (size_t)0);
	KUNIT_EXPECT_FALSE(test, ksmbd_notify_events_pending_test(notify));
}

static struct kunit_case ksmbd_notify_test_cases[] = {
	KUNIT_CASE(ksmbd_notify_test_queue_limit),
	KUNIT_CASE(ksmbd_notify_test_oversized_event),
	KUNIT_CASE(ksmbd_notify_test_request_budget),
	KUNIT_CASE(ksmbd_notify_test_cancel_overflow),
	KUNIT_CASE(ksmbd_notify_test_requeue_budget),
	KUNIT_CASE(ksmbd_notify_test_wake_overflow),
	KUNIT_CASE(ksmbd_notify_test_rename_limit),
	KUNIT_CASE(ksmbd_notify_test_filtered_rename),
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
