/*
 * Endpoint rendezvous (ADR 0009, step 5.3): data words only.
 *
 *   send(ep, info):  needs WRITE. Capture and validate the message. If a
 *                    receiver waits: deliver to the head receiver. Else if
 *                    nobody can ever receive: T_EDEAD. Else block
 *                    (BLOCKED_SEND) with the message held in the TCB.
 *   recv(ep):        needs READ, an IPC buffer, no reply authority held. If
 *                    a sender waits: take the head sender's captured
 *                    message. Else if nobody can ever send: T_EDEAD. Else
 *                    block (BLOCKED_RECV).
 *
 * Each operation: capture -> validate -> commit. Nothing is dequeued,
 * written or woken until every check has passed (I-A1, P47).
 */

#include <tarka/endpoint.h>

#include "cap_internal.h"

#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/kernel.h>
#include <tarka/string.h>
#include <tarka/syscall.h>

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

/* P3: the shape of a message, before anything is read or written. In
 * step 5.3, messages carry no capabilities. */
static int validate_info(uint64_t info)
{
    if ((info & MSG_RESERVED_IN) || MSG_LEN(info) > MSG_MAX_WORDS)
        return T_EINVAL;
    if (MSG_NCAPS(info) != 0)
        return T_EINVAL;   /* capability transfer arrives in step 5.4 */
    return T_OK;
}

/* P6, P7: capture once from the sender's IPC buffer; zero beyond the
 * length so nothing stale can travel. */
static void capture(const struct ipc_buffer *b, uint64_t info, struct ipc_msg *m)
{
    unsigned len = MSG_LEN(info);

    memset(m, 0, sizeof(*m));
    m->info = info;
    for (unsigned i = 0; i < len; i++)
        m->words[i] = b->words[i];
}

/* Commit a delivery into a receiver (cannot fail: validated by the
 * caller). Writes exactly `length` words, nothing else of the buffer. */
static void deliver(struct thread *r, const struct ipc_msg *m)
{
    struct ipc_buffer *b = ipcbuf(r);
    unsigned len = MSG_LEN(m->info);

    KASSERT(b != NULL || len == 0);
    for (unsigned i = 0; i < len; i++)
        b->words[i] = m->words[i];
    r->regs.rsi = m->info;      /* P8: info in RSI */
    r->regs.rdi = 0;            /* badges in messages: not in step 5.3 */
}

int64_t endpoint_send(struct thread *t, struct cspace *cs, cap_slot_t slot, uint64_t info)
{
    const struct cap *c;
    struct ipc_msg m;
    int rc;

    /* Validate. */
    if ((rc = cap_lookup(cs, slot, CAP_ENDPOINT, CAP_R_WRITE, &c)))
        return rc;
    if ((rc = validate_info(info)))
        return rc;
    const struct ipc_buffer *b = ipcbuf(t);
    if (!b && MSG_LEN(info) > 0)
        return T_EINVAL;   /* P4: data needs a buffer */
    struct endpoint *e = endpoint_of(c);
    struct thread *r = waitq_first(&e->receivers);
    if (!r && count_with(c, CAP_R_READ, NULL) == 0)
        return T_EDEAD;    /* nobody can ever receive (P27) */

    /* Capture (into the TCB if we may block; otherwise a local copy). */
    capture(b, info, r ? &m : &t->msg);

    /* Commit. */
    if (r) {
        KASSERT(e->senders.head == NULL);  /* never both queues (P38) */
        deliver(r, &m);
        thread_wake(r, T_OK);              /* leaves the queue: P41 */
        return T_OK;
    }
    waitq_block(t, &e->senders);
    return SYS_NORESULT;
}

int64_t endpoint_recv(struct thread *t, struct cspace *cs, cap_slot_t slot)
{
    const struct cap *c;
    int rc;

    /* Validate. */
    if ((rc = cap_lookup(cs, slot, CAP_ENDPOINT, CAP_R_READ, &c)))
        return rc;
    if (!ipcbuf(t))
        return T_EINVAL;   /* P4, P6a: receiving needs a buffer */
    if (t->reply_state != REPLY_NONE)
        return T_EBUSY;    /* P15, P17 */
    struct endpoint *e = endpoint_of(c);
    struct thread *s = waitq_first(&e->senders);
    if (!s && count_with(c, CAP_R_WRITE, NULL) == 0)
        return T_EDEAD;    /* nobody can ever send (P27) */

    /* Commit. */
    if (s) {
        KASSERT(e->receivers.head == NULL);
        deliver(t, &s->msg);
        thread_wake(s, T_OK);   /* leaving the queue consumes s->msg (P42) */
        return T_OK;
    }
    waitq_block(t, &e->receivers);
    return SYS_NORESULT;
}
