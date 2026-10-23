#ifndef TARKA_NOTIFICATION_H
#define TARKA_NOTIFICATION_H

#include <tarka/cap.h>
#include <tarka/thread.h>

/*
 * Notifications (ADR 0009, P20-P22, P27, P27a): a word of pending bits and
 * a FIFO of waiting threads. A tiny synchronisation primitive, not a
 * message queue: signals of a pending bit coalesce, and there is no count.
 */

struct notification {
    uint64_t     pending;
    struct waitq waiters;      /* kind = THREAD_BLOCKED_NOTIF */
};
_Static_assert(sizeof(struct notification) <= 64, "notification must fit its 64-byte object");

void notification_boot(void);
void notification_obj_init(paddr_t obj);

struct notification *notification_of(const struct cap *c);

/* Number of live capability records conferring signal authority (WRITE)
 * on the notification `c` refers to (P27a). */
unsigned notification_signal_caps(const struct cap *c);

/* Syscall bodies. `t` is the calling thread; results that are not the
 * status go into its saved RSI (P8). */
int64_t notification_signal(struct thread *t, struct cspace *cs, cap_slot_t slot);
int64_t notification_wait(struct thread *t, struct cspace *cs, cap_slot_t slot, bool block);

#endif
