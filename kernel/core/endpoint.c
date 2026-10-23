/*
 * Endpoint rendezvous (ADR 0009, steps 5.3 and 5.4): data words and
 * capability transfer.
 *
 *   send(ep, info):  needs WRITE (and GRANT to carry capabilities).
 *                    Capture and validate the message. If a receiver
 *                    waits: transfer and deliver to the head receiver.
 *                    Else if nobody can ever receive: T_EDEAD. Else block
 *                    (BLOCKED_SEND) with the message held in the TCB.
 *   recv(ep):        needs READ, an IPC buffer, no reply authority held.
 *                    Capture and validate the offer of receive slots. If
 *                    a sender waits: transfer and take its captured
 *                    message (a head sender whose message no longer
 *                    validates is failed instead, P53). Else if nobody
 *                    can ever send: T_EDEAD. Else block (BLOCKED_RECV).
 *
 * Each operation: capture -> validate -> commit. Nothing is dequeued,
 * derived, written or woken until every check has passed (I-A1, P47,
 * P56).
 */

#include <tarka/endpoint.h>

#include "cap_internal.h"

#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/kernel.h>
#include <tarka/string.h>
#include <tarka/syscall.h>
#include <tarka/untyped.h>

struct endpoint *endpoint_of(const struct cap *c)
{
    return phys_to_virt(cap_obj_paddr(c));
}

void endpoint_obj_init(paddr_t obj)
{
    struct endpoint *e = phys_to_virt(obj);

    /* Zeroed by retype (O6); only the queue kinds need setting. */
    e->senders.kind = THREAD_BLOCKED_SEND;
    e->receivers.kind = THREAD_BLOCKED_RECV;
}

/* P27a: computed, not cached; walks only this object's capabilities. */
static unsigned count_with(const struct cap *c, cap_rights_t right, const struct cap *except)
{
    unsigned n = 0;

    for (struct cap *x = cap_object_first((struct cap *)c); cap_same_object(x, c); x = x->next)
        if (x != except && (x->rights & right))
            n++;
    return n;
}

unsigned endpoint_caps_with(const struct cap *c, cap_rights_t right)
{
    return count_with(c, right, NULL);
}

/* Per-capability cleanup (P27, P43, P44). A blocked sender waits for a
 * receiver, so it is woken when the last receive authority goes; a
 * blocked receiver waits for a sender, so it is woken when the last send
 * authority goes. */
static void endpoint_cleanup(struct cap *c)
{
    struct endpoint *e = endpoint_of(c);

    if ((c->rights & CAP_R_READ) && count_with(c, CAP_R_READ, c) == 0)
        waitq_wake_all(&e->senders, T_EDEAD);
    if ((c->rights & CAP_R_WRITE) && count_with(c, CAP_R_WRITE, c) == 0)
        waitq_wake_all(&e->receivers, T_EDEAD);
}

static void endpoint_finalise(enum cap_type type, uint64_t object)
{
    struct endpoint *e = phys_to_virt(pa(object & ~CAP_OBJ_BITS_MASK));

    (void)type;
    /* The final capability's cleanup hook has already run, and it held
     * whatever authority remained, so P43/P44 emptied both queues. The
     * wakes below are defence in depth (P26). */
    KASSERT(!e->senders.head && !e->receivers.head);
    waitq_wake_all(&e->senders, T_EDEAD);
    waitq_wake_all(&e->receivers, T_EDEAD);
}

void endpoint_boot(void)
{
    cap_set_cleanup(CAP_ENDPOINT, endpoint_cleanup);
    cap_set_finaliser(CAP_ENDPOINT, endpoint_finalise);
}

static struct ipc_buffer *ipcbuf(struct thread *t)
{
    if (t->bind[TS_IPCBUF].type != CAP_FRAME)
        return NULL;
    return phys_to_virt(cap_obj_paddr(&t->bind[TS_IPCBUF]));
}

/* P3: the shape of a message, before anything is read or written. */
static int validate_info(uint64_t info)
{
    if ((info & MSG_RESERVED_IN) || MSG_LEN(info) > MSG_MAX_WORDS)
        return T_EINVAL;
    return T_OK;   /* MSG_NCAPS is at most 3 by construction (2 bits) */
}

/* P6, P7, P50: capture once from the sender's IPC buffer; zero beyond the
 * length and the capability count so nothing stale can travel. */
static void capture(const struct ipc_buffer *b, uint64_t info, uint64_t badge,
                    struct ipc_msg *m)
{
    unsigned len = MSG_LEN(info), nc = MSG_NCAPS(info);

    memset(m, 0, sizeof(*m));
    m->info = info;
    m->badge = badge;   /* P57 */
    for (unsigned i = 0; i < len; i++)
        m->words[i] = b->words[i];
    for (unsigned i = 0; i < nc; i++)
        m->caps[i] = b->caps[i];
}

/* P50: capture a receiver's offer of empty slots in its cspace and
 * validate it as a set. */
static int capture_offer(const struct ipc_buffer *b, const struct cspace *cs,
                         struct ipc_offer *o)
{
    uint32_t n = b->recv_count;

    memset(o, 0, sizeof(*o));
    if (n > MSG_MAX_CAPS || b->reserved)
        return T_EINVAL;
    for (uint32_t i = 0; i < n; i++)
        o->slots[i] = b->recv_slots[i];
    o->count = n;
    for (uint32_t i = 0; i < n; i++) {
        const struct cap *d = cap_slot(cs, o->slots[i]);
        if (!d)
            return T_EINVAL;
        for (uint32_t j = 0; j < i; j++)
            if (o->slots[j] == o->slots[i])
                return T_EINVAL;
        if (d->type != CAP_NULL)
            return T_EBUSY;
    }
    return T_OK;
}

/* A transfer that has passed every check (P49): commit cannot fail. */
struct xfer_plan {
    unsigned n;
    struct cap *src[MSG_MAX_CAPS];
    struct cap *dst[MSG_MAX_CAPS];
};

/* P51: the sender's listed capabilities, looked up now in its cspace. */
static int check_sources(const struct cspace *cs, const struct ipc_msg *m, struct xfer_plan *p)
{
    memset(p, 0, sizeof(*p));
    p->n = MSG_NCAPS(m->info);
    for (unsigned i = 0; i < p->n; i++) {
        struct cap *s = cap_slot(cs, m->caps[i]);
        int rc;
        if (!s)
            return T_ERANGE;
        if ((rc = cap_derive_check(s, NULL, 0)))
            return rc;
        p->src[i] = s;
    }
    return T_OK;
}

/* P52: does the planned transfer fit this offer, in this cspace, now? */
static int check_fit(const struct cspace *cs, const struct ipc_offer *o, struct xfer_plan *p)
{
    if (p->n > o->count)
        return T_EINVAL;
    for (unsigned i = 0; i < p->n; i++) {
        struct cap *d = cap_slot(cs, o->slots[i]);
        int rc;
        if (!d)
            return T_EINVAL;
        if ((rc = cap_derive_check(p->src[i], d, 0)))
            return rc;   /* T_EBUSY: filled since it was offered (P24) */
        p->dst[i] = d;
    }
    return T_OK;
}

/* A thread's own cspace, from its binding. A blocked thread always has
 * one: losing it stops the thread (T4). */
static int thread_cspace(struct thread *t, struct cspace *out)
{
    return cnode_view_cap(&t->bind[TS_CSPACE], out);
}

#define INFO_REPLY_SKIPPED (1ull << 10)   /* P19, P63 */

/* Commit a delivery into a receiver (cannot fail: validated by the
 * caller). P54: derive, then write exactly `length` words, nothing else of
 * the buffer; RSI = info with the delivered count (and `extra` bits), RDI
 * = the sender's badge (P57; 0 for a reply). */
static void deliver(struct thread *r, const struct ipc_msg *m, const struct xfer_plan *p,
                    uint64_t extra)
{
    struct ipc_buffer *b = ipcbuf(r);
    unsigned len = MSG_LEN(m->info);

    KASSERT(b != NULL || len == 0);
    KASSERT(p->n == MSG_NCAPS(m->info));
    for (unsigned i = 0; i < p->n; i++)
        cap_derive_commit(p->src[i], p->dst[i], CAP_R_ALL, 0);
    for (unsigned i = 0; i < len; i++)
        b->words[i] = m->words[i];
    r->regs.rsi = m->info | ((uint64_t)p->n << 8) | extra;
    r->regs.rdi = m->badge;
}

/* send and call (P13, P51, P58). */
static int64_t send_common(struct thread *t, struct cspace *cs, cap_slot_t slot,
                           uint64_t info, bool call)
{
    const struct cap *c;
    struct ipc_msg m;
    struct ipc_offer o;
    struct xfer_plan p;
    int rc;

    /* Validate. */
    if ((rc = cap_lookup(cs, slot, CAP_ENDPOINT, CAP_R_WRITE, &c)))
        return rc;
    if ((rc = validate_info(info)))
        return rc;
    const struct ipc_buffer *b = ipcbuf(t);
    if (!b && (MSG_LEN(info) > 0 || MSG_NCAPS(info) > 0))
        return T_EINVAL;   /* P4: data and capabilities need a buffer */
    bool grant = (c->rights & CAP_R_GRANT) != 0;
    if (MSG_NCAPS(info) > 0 && !grant)
        return T_EPERM;    /* P51 */
    memset(&o, 0, sizeof(o));
    if (call && b && (rc = capture_offer(b, cs, &o)))
        return rc;         /* P58: the caller's offer for the reply */
    struct endpoint *e = endpoint_of(c);
    struct thread *r = waitq_first(&e->receivers);
    if (!r && count_with(c, CAP_R_READ, NULL) == 0)
        return T_EDEAD;    /* nobody can ever receive (P27) */

    capture(b, info, c->badge, &m);
    if ((rc = check_sources(cs, &m, &p)))
        return rc;

    if (r) {
        struct cspace rcs;
        if ((rc = thread_cspace(r, &rcs)))
            return rc;
        if ((rc = check_fit(&rcs, &r->offer, &p)))
            return rc;     /* P53: the receiver stays as it was */

        /* Commit. */
        KASSERT(e->senders.head == NULL);  /* never both queues (P38) */
        uint64_t extra = (r->ipc_flags & IPC_REPLY_SKIPPED) ? INFO_REPLY_SKIPPED : 0;
        deliver(r, &m, &p, extra);
        thread_wake(r, T_OK);              /* leaves the queue: P41 */
        if (!call)
            return T_OK;
        t->offer = o;
        ipc_call_link(r, t, grant);        /* straight to BLOCKED_REPLY */
        return SYS_NORESULT;
    }
    /* Commit: block with the message (and a call's offer and flags) held
     * in the TCB (P46, P58). */
    t->msg = m;
    if (call) {
        t->offer = o;
        t->ipc_flags = IPC_CALL | (grant ? IPC_CALL_GRANT : 0);
    }
    waitq_block(t, &e->senders);
    return SYS_NORESULT;
}

int64_t endpoint_send(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info)
{
    return send_common(t, cs, slot, info, false);
}

int64_t endpoint_call(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info)
{
    return send_common(t, cs, slot, info, true);
}

/* The receive half (P50, P63 phase 1): READ, a buffer, no reply
 * authority held (for a plain recv), then the offer. */
static int recv_prepare(struct thread *t, struct cspace *cs, cap_slot_t slot, bool plain,
                        const struct cap **c, struct ipc_offer *o)
{
    int rc;

    if ((rc = cap_lookup(cs, slot, CAP_ENDPOINT, CAP_R_READ, c)))
        return rc;
    const struct ipc_buffer *b = ipcbuf(t);
    if (!b)
        return T_EINVAL;   /* P4, P6a: receiving needs a buffer */
    if (plain && t->reply_state != REPLY_NONE)
        return T_EBUSY;    /* P15, P17 */
    return capture_offer(b, cs, o);
}

/* P53: fail queued senders whose message can no longer be delivered into
 * offer `o` in cspace `cs`, until the head is deliverable. Returns it,
 * with its plan in `p`, or NULL. One of the two queue-draining operations
 * of P37; each failed sender leaves exactly once (I-E2). */
static struct thread *drain(struct endpoint *e, const struct cspace *cs,
                            const struct ipc_offer *o, struct xfer_plan *p)
{
    struct thread *s;

    while ((s = waitq_first(&e->senders))) {
        struct cspace scs;
        int rc = thread_cspace(s, &scs);
        if (!rc)
            rc = check_sources(&scs, &s->msg, p);
        if (!rc)
            rc = check_fit(cs, o, p);
        if (!rc)
            return s;
        thread_wake(s, rc);
    }
    return NULL;
}

/* Commit a receive: take deliverable head sender `s` (plan `p`), or block
 * with the offer held in the TCB. */
static int64_t recv_commit(struct thread *t, struct endpoint *e, struct thread *s,
                           const struct xfer_plan *p, const struct ipc_offer *o, bool skipped)
{
    if (s) {
        KASSERT(e->receivers.head == NULL);
        deliver(t, &s->msg, p, skipped ? INFO_REPLY_SKIPPED : 0);
        if (s->ipc_flags & IPC_CALL)
            ipc_call_link(t, s, (s->ipc_flags & IPC_CALL_GRANT) != 0);   /* P58 */
        else
            thread_wake(s, T_OK);   /* leaving the queue consumes s->msg (P42) */
        return T_OK;
    }
    t->offer = *o;
    t->ipc_flags = skipped ? IPC_REPLY_SKIPPED : 0;
    waitq_block(t, &e->receivers);
    return SYS_NORESULT;
}

int64_t endpoint_recv(struct thread *t, struct cspace *cs, cap_slot_t slot)
{
    const struct cap *c;
    struct ipc_offer o;
    struct xfer_plan p;
    int rc;

    if ((rc = recv_prepare(t, cs, slot, true, &c, &o)))
        return rc;
    struct endpoint *e = endpoint_of(c);
    struct thread *s = drain(e, cs, &o, &p);
    if (!s && count_with(c, CAP_R_WRITE, NULL) == 0)
        return T_EDEAD;    /* nobody can ever send (P27) */
    return recv_commit(t, e, s, &p, &o, false);
}

/* P61 step 3: check a reply from server `t` to its pending caller. */
static int reply_check(struct thread *t, const struct cspace *cs, uint64_t info,
                       struct ipc_msg *m, struct xfer_plan *p)
{
    struct thread *caller = t->reply_to;
    struct cspace ccs;
    int rc;

    KASSERT(t->reply_state == REPLY_PENDING && caller);
    if ((rc = validate_info(info)))
        return rc;
    const struct ipc_buffer *b = ipcbuf(t);
    if (!b && (MSG_LEN(info) > 0 || MSG_NCAPS(info) > 0))
        return T_EINVAL;
    if (MSG_NCAPS(info) > 0 && !(t->reply_flags & REPLY_CALL_GRANT))
        return T_EPERM;    /* P23a: only what call_grant allows */
    capture(b, info, 0, m);
    if ((rc = check_sources(cs, m, p)))
        return rc;
    if (MSG_LEN(info) > 0 && !ipcbuf(caller))
        return T_EINVAL;   /* P6a, P18: the caller cannot take data */
    if ((rc = thread_cspace(caller, &ccs)))
        return rc;
    return check_fit(&ccs, &caller->offer, p);
}

int64_t endpoint_reply(struct thread *t, struct cspace *cs, uint64_t info)
{
    struct ipc_msg m;
    struct xfer_plan p;
    int rc;

    if (t->reply_state == REPLY_NONE)
        return T_EINVAL;
    if (t->reply_state == REPLY_CALLER_GONE) {
        reply_consume_gone(t);
        return T_EDEAD;
    }
    if ((rc = reply_check(t, cs, info, &m, &p)))
        return rc;         /* the authority is kept (P18) */
    deliver(t->reply_to, &m, &p, 0);
    reply_complete(t);
    return T_OK;
}

int64_t endpoint_reply_recv(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info)
{
    const struct cap *c;
    struct ipc_offer o;
    struct ipc_msg rm;
    struct xfer_plan rp, p;
    int rc;

    /* Phase 1: the receive half. */
    if ((rc = recv_prepare(t, cs, slot, false, &c, &o)))
        return rc;
    /* Phase 2: the reply half, and the two halves' slots kept apart. */
    bool pending = t->reply_state == REPLY_PENDING;
    if (pending) {
        if ((rc = reply_check(t, cs, info, &rm, &rp)))
            return rc;
        for (unsigned i = 0; i < rp.n; i++)
            for (uint32_t j = 0; j < o.count; j++)
                if (rp.dst[i] == cap_slot(cs, o.slots[j]))
                    return T_EBUSY;
    }
    /* Phases 3 and 4: drain, then liveness, before anything of ours. */
    struct endpoint *e = endpoint_of(c);
    struct thread *s = drain(e, cs, &o, &p);
    if (!s && count_with(c, CAP_R_WRITE, NULL) == 0)
        return T_EDEAD;
    /* Phase 5: commit the reply, then receive. */
    bool skipped = false;
    if (pending) {
        deliver(t->reply_to, &rm, &rp, 0);
        reply_complete(t);
    } else if (t->reply_state == REPLY_CALLER_GONE) {
        reply_consume_gone(t);
        skipped = true;
    }
    if (s) {
        /* The reply's commit cannot have invalidated the head (P63). */
        struct cspace scs;
        KASSERT(thread_cspace(s, &scs) == T_OK && check_sources(&scs, &s->msg, &p) == T_OK &&
                check_fit(cs, &o, &p) == T_OK);
    }
    return recv_commit(t, e, s, &p, &o, skipped);
}
