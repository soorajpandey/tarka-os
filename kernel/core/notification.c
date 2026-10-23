/*
 * Notifications (ADR 0009, P20-P22, P27, P27a).
 *
 *   signal(cap):  pending |= cap.badge
 *                 if a thread waits: the first waiter gets all pending
 *                 bits, pending = 0, the waiter becomes RUNNABLE
 *                 otherwise: the bits stay pending
 *   wait(cap):    pending != 0: return pending, pending = 0
 *                 nobody can ever signal: T_EDEAD
 *                 otherwise: block (BLOCKED_NOTIF)
 *   poll(cap):    like wait, but returns 0 instead of blocking
 */

#include <tarka/notification.h>

#include "cap_internal.h"

#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/kernel.h>
#include <tarka/syscall.h>

struct notification *notification_of(const struct cap *c)
{
    return phys_to_virt(cap_obj_paddr(c));
}

void notification_obj_init(paddr_t obj)
{
    struct notification *n = phys_to_virt(obj);

    /* Zeroed by retype (O6); only the queue kind needs setting. */
    n->waiters.kind = THREAD_BLOCKED_NOTIF;
}

/* P27a: a semantic count, computed rather than cached. All capabilities to
 * one object are adjacent in the CDT (one root per object, every copy a
 * descendant), so this walks only this object's capabilities. */
static unsigned count_write(const struct cap *c, const struct cap *except)
{
    unsigned n = 0;

    for (struct cap *x = cap_object_first((struct cap *)c); cap_same_object(x, c); x = x->next)
        if (x != except && (x->rights & CAP_R_WRITE))
            n++;
    return n;
}

unsigned notification_signal_caps(const struct cap *c)
{
    return count_write(c, NULL);
}

/* Per-capability cleanup: if this is the last capability that confers
 * signal authority, every waiter is woken with T_EDEAD before the
 * deletion completes (P27, I-L1). */
static void notification_cleanup(struct cap *c)
{
    if ((c->rights & CAP_R_WRITE) && count_write(c, c) == 0)
        waitq_wake_all(&notification_of(c)->waiters, T_EDEAD);
}

/* Last capability deleted: nobody is waiting any more (O10). */
static void notification_finalise(enum cap_type type, uint64_t object)
{
    struct notification *n = phys_to_virt(pa(object & ~CAP_OBJ_BITS_MASK));

    (void)type;
    waitq_wake_all(&n->waiters, T_EDEAD);
}

void notification_boot(void)
{
    cap_set_cleanup(CAP_NOTIFICATION, notification_cleanup);
    cap_set_finaliser(CAP_NOTIFICATION, notification_finalise);
}

int64_t notification_signal(struct thread *t, struct cspace *cs, cap_slot_t slot)
{
    const struct cap *c;
    int rc;

    (void)t;
    /* Validate everything first; the commit below cannot fail. */
    if ((rc = cap_lookup(cs, slot, CAP_NOTIFICATION, CAP_R_WRITE, &c)))
        return rc;
    if (c->badge == 0)
        return T_EPERM;     /* P20: signal authority is always a specific badge */

    struct notification *n = notification_of(c);
    n->pending |= c->badge;
    struct thread *w = waitq_first(&n->waiters);
    if (w) {
        w->regs.rsi = n->pending;   /* P8: bits in RSI, status in RAX */
        n->pending = 0;
        thread_wake(w, T_OK);
    }
    return T_OK;
}

int64_t notification_wait(struct thread *t, struct cspace *cs, cap_slot_t slot, bool block)
{
    const struct cap *c;
    int rc;

    if ((rc = cap_lookup(cs, slot, CAP_NOTIFICATION, CAP_R_READ, &c)))
        return rc;

    struct notification *n = notification_of(c);
    if (n->pending || !block) {
        t->regs.rsi = n->pending;   /* consumed exactly here (P22) */
        n->pending = 0;
        return T_OK;
    }
    if (count_write(c, NULL) == 0)
        return T_EDEAD;             /* nobody can ever signal (P27) */
    waitq_block(t, &n->waiters);
    return SYS_NORESULT;
}
