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
static void capture(const struct ipc_buffer *b, uint64_t info, struct ipc_msg *m)
{
    unsigned len = MSG_LEN(info), nc = MSG_NCAPS(info);

    memset(m, 0, sizeof(*m));
    m->info = info;
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

/* Commit a rendezvous into a receiver (cannot fail: validated by the
 * caller). P54: derive, then write exactly `length` words, nothing else of
 * the buffer. */
static void deliver(struct thread *r, const struct ipc_msg *m, const struct xfer_plan *p)
{
    struct ipc_buffer *b = ipcbuf(r);
    unsigned len = MSG_LEN(m->info);

    KASSERT(b != NULL || len == 0);
    KASSERT(p->n == MSG_NCAPS(m->info));
    for (unsigned i = 0; i < p->n; i++)
        cap_derive_commit(p->src[i], p->dst[i], CAP_R_ALL, 0);
    for (unsigned i = 0; i < len; i++)
        b->words[i] = m->words[i];
    r->regs.rsi = m->info | ((uint64_t)p->n << 8);   /* P8: delivered count */
    r->regs.rdi = 0;            /* badges in messages: not in step 5.4 */
}

int64_t endpoint_send(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info)
{
    const struct cap *c;
    struct ipc_msg m;
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
    if (MSG_NCAPS(info) > 0 && !(c->rights & CAP_R_GRANT))
        return T_EPERM;    /* P51 */
    struct endpoint *e = endpoint_of(c);
    struct thread *r = waitq_first(&e->receivers);
    if (!r && count_with(c, CAP_R_READ, NULL) == 0)
        return T_EDEAD;    /* nobody can ever receive (P27) */

    capture(b, info, &m);
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
        deliver(r, &m, &p);
        thread_wake(r, T_OK);              /* leaves the queue: P41 */
        return T_OK;
    }
    /* Commit: block with the message held in the TCB (P46). */
    t->msg = m;
    waitq_block(t, &e->senders);
    return SYS_NORESULT;
}

int64_t endpoint_recv(struct thread *t, struct cspace *cs, cap_slot_t slot)
{
    const struct cap *c;
    struct ipc_offer o;
    struct xfer_plan p;
    int rc;

    /* Validate. */
    if ((rc = cap_lookup(cs, slot, CAP_ENDPOINT, CAP_R_READ, &c)))
        return rc;
    const struct ipc_buffer *b = ipcbuf(t);
    if (!b)
        return T_EINVAL;   /* P4, P6a: receiving needs a buffer */
    if (t->reply_state != REPLY_NONE)
        return T_EBUSY;    /* P15, P17 */
    if ((rc = capture_offer(b, cs, &o)))
        return rc;         /* P50: the receiver's own error */
    struct endpoint *e = endpoint_of(c);

    for (;;) {
        struct thread *s = waitq_first(&e->senders);
        if (!s)
            break;
        struct cspace scs;
        rc = thread_cspace(s, &scs);
        if (!rc)
            rc = check_sources(&scs, &s->msg, &p);
        if (!rc)
            rc = check_fit(cs, &o, &p);
        if (rc) {
            /* P53: the head sender's own message no longer validates.
             * It leaves with its error; it cannot wedge this receiver. */
            thread_wake(s, rc);
            continue;
        }
        /* Commit. */
        KASSERT(e->receivers.head == NULL);
        deliver(t, &s->msg, &p);
        thread_wake(s, T_OK);   /* leaving the queue consumes s->msg (P42) */
        return T_OK;
    }

    if (count_with(c, CAP_R_WRITE, NULL) == 0)
        return T_EDEAD;    /* nobody can ever send (P27) */
    /* Commit: block with the offer held in the TCB (P50). */
    t->offer = o;
    waitq_block(t, &e->receivers);
    return SYS_NORESULT;
}
