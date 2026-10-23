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

#include <tarka/cont.h>
#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/kernel.h>
#include <tarka/reaper.h>
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

_Static_assert(sizeof(struct endpoint) <= (1u << ENDPOINT_BITS), "endpoint must fit its object");

/* P27a as amended by ADR 0010 SC17: maintained, not computed. */
static unsigned count_with(const struct cap *c, cap_rights_t right)
{
    const struct endpoint *e = endpoint_of(c);

    KASSERT(right == CAP_R_READ || right == CAP_R_WRITE);
    return right == CAP_R_READ ? e->meta.n_read : e->meta.n_write;
}

unsigned endpoint_caps_with(const struct cap *c, cap_rights_t right)
{
    return count_with(c, right);
}

/* Per-capability cleanup (P27, P43, P44). A blocked sender waits for a
 * receiver, so it is woken when the last receive authority goes; a
 * blocked receiver waits for a sender, so it is woken when the last send
 * authority goes. The counts still include `c` (it is uncounted in the
 * step that unlinks it), so `c` is the last with a right when the count
 * is 1. */
static void endpoint_cleanup(struct cap *c)
{
    struct endpoint *e = endpoint_of(c);

    /* Authority loss (SC23): the doomed waiters are failed by the reaper,
     * one per unit, not all in this entry. A count that reaches 0 never
     * rises again, so a doomed queue only shrinks. */
    if ((c->rights & CAP_R_READ) && count_with(c, CAP_R_READ) == 1 && e->senders.head)
        reaper_doom(c, &e->meta, OBJ_DOOM_SEND);
    if ((c->rights & CAP_R_WRITE) && count_with(c, CAP_R_WRITE) == 1 && e->receivers.head)
        reaper_doom(c, &e->meta, OBJ_DOOM_RECV);
}

static void endpoint_finalise(enum cap_type type, uint64_t object)
{
    struct endpoint *e = phys_to_virt(pa(object & ~CAP_OBJ_BITS_MASK));

    (void)type;
    /* Finalised at once only when nobody waits (an endpoint with waiters
     * dies through the reaper, SC23), so there is nothing to wake. */
    KASSERT(!e->senders.head && !e->receivers.head);
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
    if (!r && count_with(c, CAP_R_READ) == 0)
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
        ipc_call_link(r, t, grant ? REPLY_CALL_GRANT : 0);   /* to BLOCKED_REPLY */
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

/* P53: can head sender `s` deliver into offer `o` in cspace `cs` now?
 * T_OK with its plan in `p`, or the error it is failed with. The plan
 * holds slot pointers, so it is rebuilt in every chunk, never kept
 * (E0). */
static int deliverable(struct thread *s, const struct cspace *cs, const struct ipc_offer *o,
                       struct xfer_plan *p)
{
    struct cspace scs;
    int rc = thread_cspace(s, &scs);

    if (!rc)
        rc = check_sources(&scs, &s->msg, p);
    if (!rc)
        rc = check_fit(cs, o, p);
    return rc;
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
            ipc_call_link(t, s, ((s->ipc_flags & IPC_CALL_GRANT) ? REPLY_CALL_GRANT : 0) |
                                ((s->ipc_flags & IPC_FAULT) ? REPLY_FAULT : 0));   /* P58, P69 */
        else
            thread_wake(s, T_OK);   /* leaving the queue consumes s->msg (P42) */
        return T_OK;
    }
    t->offer = *o;
    t->ipc_flags = skipped ? IPC_REPLY_SKIPPED : 0;
    waitq_block(t, &e->receivers);
    return SYS_NORESULT;
}

/* SC25, E7: capture once (the slot, the endpoint's identity, the offer),
 * then the drain runs as a continuation, first chunk here. */
int64_t endpoint_recv(struct thread *t, struct cspace *cs, cap_slot_t slot)
{
    const struct cap *c;
    struct ipc_offer o;
    int rc;

    if ((rc = recv_prepare(t, cs, slot, true, &c, &o)))
        return rc;
    t->offer = o;
    t->cont = (struct thread_cont){ .kind = CONT_RECV, .slot = slot, .type = c->type,
                                    .object = c->object, .serial = obj_serial(c) };
    return cont_after_first(t, endpoint_drain_chunk(t, cs));
}

/* P61 step 3, split in two (ADR 0010 E7). Capture reads the server's IPC
 * buffer, once; validation works on the kernel copy and can be re-run at
 * the commit point of a chunked reply_recv. */
static int reply_capture(struct thread *t, uint64_t info, struct ipc_msg *m)
{
    int rc;

    KASSERT(t->reply_state == REPLY_PENDING && t->reply_to);
    if ((rc = validate_info(info)))
        return rc;
    if (t->reply_flags & REPLY_FAULT) {
        /* P69: a fault reply is a label only: it never writes to the
         * faulting thread's buffer and carries no capabilities. */
        if (MSG_NCAPS(info) > 0)
            return T_EPERM;
        if (MSG_LEN(info) > 0)
            return T_EINVAL;
        memset(m, 0, sizeof(*m));
        m->info = info;
        return T_OK;
    }
    const struct ipc_buffer *b = ipcbuf(t);
    if (!b && (MSG_LEN(info) > 0 || MSG_NCAPS(info) > 0))
        return T_EINVAL;
    if (MSG_NCAPS(info) > 0 && !(t->reply_flags & REPLY_CALL_GRANT))
        return T_EPERM;    /* P23a: only what call_grant allows */
    capture(b, info, 0, m);
    return T_OK;
}

static int reply_validate(struct thread *t, const struct cspace *cs, const struct ipc_msg *m,
                          struct xfer_plan *p)
{
    struct thread *caller = t->reply_to;
    struct cspace ccs;
    int rc;

    KASSERT(t->reply_state == REPLY_PENDING && caller);
    if (t->reply_flags & REPLY_FAULT) {
        memset(p, 0, sizeof(*p));
        return T_OK;
    }
    if ((rc = check_sources(cs, m, p)))
        return rc;
    if (MSG_LEN(m->info) > 0 && !ipcbuf(caller))
        return T_EINVAL;   /* P6a, P18: the caller cannot take data */
    if ((rc = thread_cspace(caller, &ccs)))
        return rc;
    return check_fit(&ccs, &caller->offer, p);
}

static int reply_check(struct thread *t, const struct cspace *cs, uint64_t info,
                       struct ipc_msg *m, struct xfer_plan *p)
{
    int rc = reply_capture(t, info, m);

    return rc ? rc : reply_validate(t, cs, m, p);
}

/* P63 phase 2: the reply's capabilities never land in a slot the receive
 * half offered. */
static int slots_apart(const struct cspace *cs, const struct xfer_plan *rp, const struct ipc_offer *o)
{
    for (unsigned i = 0; i < rp->n; i++)
        for (uint32_t j = 0; j < o->count; j++)
            if (rp->dst[i] == cap_slot(cs, o->slots[j]))
                return T_EBUSY;
    return T_OK;
}

/* P61 step 4, P71: commit a checked reply. A fault caller gets no
 * delivery: label 0 resumes it, any other label leaves it FAULTED. */
static void reply_commit(struct thread *t, const struct ipc_msg *m, const struct xfer_plan *p)
{
    if (t->reply_flags & REPLY_FAULT) {
        reply_complete(t, MSG_LABEL(m->info) == 0 ? T_OK : T_EINVAL);
        return;
    }
    deliver(t->reply_to, m, p, 0);
    reply_complete(t, T_OK);
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
    reply_commit(t, &m, &p);
    return T_OK;
}

/* SC25, E2, E7: validate both halves and capture once (the slot and
 * identity, the offer, the reply message, and k, the endpoint's own count
 * of queued senders); then the drain runs as a continuation, first chunk
 * here. */
int64_t endpoint_reply_recv(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info)
{
    const struct cap *c;
    struct ipc_offer o;
    struct ipc_msg rm;
    struct xfer_plan rp;
    int rc;

    /* Phase 1: the receive half. */
    if ((rc = recv_prepare(t, cs, slot, false, &c, &o)))
        return rc;
    /* Phase 2: the reply half, and the two halves' slots kept apart. */
    bool pending = t->reply_state == REPLY_PENDING;
    if (pending) {
        if ((rc = reply_check(t, cs, info, &rm, &rp)))
            return rc;
        if ((rc = slots_apart(cs, &rp, &o)))
            return rc;
    }
    t->offer = o;
    if (pending)
        t->msg = rm;
    t->cont = (struct thread_cont){ .kind = CONT_REPLY_RECV, .slot = slot, .type = c->type,
                                    .object = c->object, .serial = obj_serial(c),
                                    .aux = endpoint_of(c)->senders.len };
    return cont_after_first(t, endpoint_drain_chunk(t, cs));
}

/* E0: the receive's endpoint, found again by slot and identity, with what
 * a receive needs. A different endpoint in that slot is not the one
 * named: T_EBADCAP, never an operation on it. */
static int recv_resolve(struct thread *t, struct cspace *cs, const struct thread_cont *k,
                        bool plain, const struct cap **c)
{
    int rc;

    if ((rc = cap_lookup(cs, k->slot, CAP_ENDPOINT, CAP_R_READ, c)))
        return rc;
    if (!obj_identity_is(*c, k->type, k->object, k->serial))
        return T_EBADCAP;
    if (!ipcbuf(t))
        return T_EINVAL;   /* P4, P6a: unbound between chunks */
    if (plain && t->reply_state != REPLY_NONE)
        return T_EBUSY;
    return T_OK;
}

#ifdef TARKA_SELFTEST
uint64_t drain_failed, drain_failed_at_reply;   /* E2 witnesses (tests) */
#endif

/* One chunk (SC25, E2, E7). Fails at most DRAIN_CHUNK undeliverable
 * senders, each exactly once (I-E2); before a reply_recv's reply commits,
 * only as many as its snapshot allows (E2). Then: deliver, block, or
 * (reply_recv only, snapshot spent) commit the reply and go on as a
 * plain receive. */
static int64_t drain_step(struct thread *t, struct cspace *cs)
{
    struct thread_cont *k = &t->cont;
    bool reply = k->kind == CONT_REPLY_RECV;
    const struct cap *c;
    struct xfer_plan p;
    struct thread *s;
    int rc;

    if ((rc = recv_resolve(t, cs, k, !reply, &c)))
        return rc;
    struct endpoint *e = endpoint_of(c);
    for (unsigned failed = 0;; failed++) {
        if (!(s = waitq_first(&e->senders)) || !(rc = deliverable(s, cs, &t->offer, &p)))
            break;
        if (reply && k->aux == 0)
            break;                    /* E2: the snapshot is spent */
        if (failed == DRAIN_CHUNK)
            return CONT_PENDING;      /* a boundary before the next ones */
        thread_wake(s, rc);           /* failed once: it leaves the queue */
#ifdef TARKA_SELFTEST
        drain_failed++;
#endif
        if (reply)
            k->aux--;
    }
    bool now = s && rc == T_OK;       /* the head can be delivered */
    if (!s && count_with(c, CAP_R_WRITE) == 0)
        return T_EDEAD;               /* nobody can ever send (P27); nothing committed */

    bool skipped = !reply && k->aux;
    if (reply) {
        /* P63 phases 2 and 5, on the captured reply. */
        if (t->reply_state == REPLY_PENDING) {
            struct xfer_plan rp;
            if ((rc = reply_validate(t, cs, &t->msg, &rp)) || (rc = slots_apart(cs, &rp, &t->offer)))
                return rc;
            reply_commit(t, &t->msg, &rp);
#ifdef TARKA_SELFTEST
            drain_failed_at_reply = drain_failed;
#endif
        } else if (t->reply_state == REPLY_CALLER_GONE) {
            reply_consume_gone(t);
            skipped = true;
        }
        memset(&t->msg, 0, sizeof(t->msg));   /* the reply is consumed */
        if (s && !now) {
            /* E2: what remains is an ordinary receive. */
            *k = (struct thread_cont){ .kind = CONT_RECV, .slot = k->slot, .type = k->type,
                                       .object = k->object, .serial = k->serial,
                                       .aux = skipped };
            return CONT_PENDING;
        }
        /* The reply's commit cannot have invalidated the head (P63). */
        KASSERT(!now || deliverable(s, cs, &t->offer, &p) == T_OK);
    }
    if (now) {
        struct ipc_offer o = t->offer;
        memset(&t->offer, 0, sizeof(t->offer));   /* delivered: nothing staged */
        return recv_commit(t, e, s, &p, &o, skipped);
    }
    return recv_commit(t, e, NULL, &p, &t->offer, skipped);   /* block with the offer */
}

int64_t endpoint_drain_chunk(struct thread *t, struct cspace *cs)
{
    int64_t rc = drain_step(t, cs);

    /* Finished without blocking: nothing captured may stay staged. */
    if (rc != CONT_PENDING && rc != SYS_NORESULT) {
        memset(&t->offer, 0, sizeof(t->offer));
        memset(&t->msg, 0, sizeof(t->msg));
    }
    return rc;
}

/* P67-P69: a ring-3 exception of the current thread `t` (its fault record
 * already written) becomes a call made by the kernel from its TS_FAULT
 * binding. Returns T_OK if `t` is now delivering (BLOCKED_SEND or
 * BLOCKED_REPLY); otherwise an error, and the caller leaves it FAULTED.
 * Writes none of `t`'s registers (P70) and touches no user address. */
int endpoint_fault(struct thread *t)
{
    const struct cap *c = &t->bind[TS_FAULT];
    struct ipc_msg m;
    struct xfer_plan p;

    KASSERT(t->state == THREAD_RUNNING);
    if (c->type != CAP_ENDPOINT || !(c->rights & CAP_R_WRITE))
        return T_EBADCAP;  /* no handler */
    struct endpoint *e = endpoint_of(c);
    struct thread *r = waitq_first(&e->receivers);
    if (!r && count_with(c, CAP_R_READ) == 0)
        return T_EDEAD;    /* nobody can ever handle it */

    /* P68: only the thread's own state; no offer, no call_grant. */
    memset(&m, 0, sizeof(m));
    m.info = ((uint64_t)FAULT_LABEL << 32) | FAULT_WORDS | INFO_FAULT;
    m.words[0] = t->fault.vector;
    m.words[1] = t->fault.error;
    m.words[2] = t->fault.addr;
    m.words[3] = t->fault.rip;
    m.words[4] = t->regs.rsp;
    m.words[5] = t->regs.rflags & RFLAGS_USER_MASK;
    m.badge = c->badge;
    memset(&p, 0, sizeof(p));

    if (r) {
        KASSERT(e->senders.head == NULL);
        uint64_t extra = (r->ipc_flags & IPC_REPLY_SKIPPED) ? INFO_REPLY_SKIPPED : 0;
        deliver(r, &m, &p, extra);
        thread_wake(r, T_OK);
        ipc_call_link(r, t, REPLY_FAULT);
        return T_OK;
    }
    t->msg = m;
    t->ipc_flags = IPC_CALL | IPC_FAULT;
    waitq_block(t, &e->senders);
    return T_OK;
}
