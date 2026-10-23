/*
 * IPC state machinery (ADR 0009, P9-P12, P26-P31).
 *
 * Kept deliberately plain: doubly linked FIFO queues through one wait link
 * per thread, a pair of reply links, and functions that move a thread
 * between states only together with every structure that records it.
 */

#include <tarka/ipc.h>

#include <tarka/endpoint.h>
#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/string.h>

#include "../arch/x86_64/percpu.h"

static bool blocked(uint32_t state)
{
    return state >= THREAD_BLOCKED_SEND && state <= THREAD_BLOCKED_NOTIF;
}

static void waitq_append(struct waitq *q, struct thread *t)
{
    KASSERT(!t->wait_q && !t->wait_next && !t->wait_prev);
    t->wait_q = q;
    t->wait_prev = q->tail;
    t->wait_next = NULL;
    if (q->tail)
        q->tail->wait_next = t;
    else
        q->head = t;
    q->tail = t;
}

static void waitq_remove(struct thread *t)
{
    struct waitq *q = t->wait_q;

    if (!q)
        return;
    if (t->wait_prev)
        t->wait_prev->wait_next = t->wait_next;
    else
        q->head = t->wait_next;
    if (t->wait_next)
        t->wait_next->wait_prev = t->wait_prev;
    else
        q->tail = t->wait_prev;
    t->wait_q = NULL;
    t->wait_next = t->wait_prev = NULL;
}

/* P42, P50, P59: staged IPC state lives exactly as long as the wait it
 * belongs to; leaving IPC by any route (wake-up, abort, destruction)
 * erases all of it. */
static void ipc_clear_staged(struct thread *t)
{
    memset(&t->msg, 0, sizeof(t->msg));
    memset(&t->offer, 0, sizeof(t->offer));
    t->ipc_flags = 0;
}

void waitq_block(struct thread *t, struct waitq *q)
{
    KASSERT(t->state == THREAD_RUNNING && this_cpu()->current == t);
    KASSERT(blocked(q->kind) && q->kind != THREAD_BLOCKED_REPLY);
    waitq_append(q, t);
    t->state = q->kind;
}

struct thread *waitq_first(const struct waitq *q)
{
    return q->head;
}

/* Undo the caller side of a reply link. */
static void reply_unlink_caller(struct thread *caller)
{
    struct thread *server = caller->reply_from;

    if (!server)
        return;
    KASSERT(server->reply_state == REPLY_PENDING && server->reply_to == caller);
    server->reply_to = NULL;
    server->reply_state = REPLY_CALLER_GONE;   /* P29: authority kept, caller gone */
    server->reply_flags = 0;                   /* P60 */
    caller->reply_from = NULL;
}

/* Is `t` blocked delivering a fault (P69, P76)? */
static bool fault_caller(const struct thread *t)
{
    if (t->state == THREAD_BLOCKED_SEND)
        return (t->ipc_flags & IPC_FAULT) != 0;
    if (t->state == THREAD_BLOCKED_REPLY)
        return t->reply_from && (t->reply_from->reply_flags & REPLY_FAULT);
    return false;
}

void thread_wake(struct thread *t, int64_t rax)
{
    KASSERT(blocked(t->state));
    bool fault = fault_caller(t);

    waitq_remove(t);
    reply_unlink_caller(t);
    ipc_clear_staged(t);
    if (fault) {
        /* P70-P72: no register is written. */
        if (rax != T_OK) {
            t->state = THREAD_FAULTED;   /* the record stays (P73) */
            return;
        }
        memset(&t->fault, 0, sizeof(t->fault));
        t->ret_path = RETURN_IRET;
    } else {
        t->regs.rax = (uint64_t)rax;   /* P10: result, state and queue together */
    }
    t->state = THREAD_RUNNABLE;
    thread_enqueue(t);
}

void waitq_wake_all(struct waitq *q, int64_t rax)
{
    struct thread *t;

    while ((t = q->head))
        thread_wake(t, rax);
}

void reply_link(struct thread *server, struct thread *caller, uint32_t flags)
{
    KASSERT(server->reply_state == REPLY_NONE && !server->reply_to);
    KASSERT(!caller->wait_q && !caller->reply_from);
    server->reply_to = caller;
    server->reply_state = REPLY_PENDING;
    server->reply_flags = flags;
    caller->reply_from = server;
    caller->state = THREAD_BLOCKED_REPLY;
}

void ipc_call_link(struct thread *server, struct thread *caller, uint32_t reply_flags)
{
    KASSERT((caller->state == THREAD_RUNNING && this_cpu()->current == caller) ||
            (caller->state == THREAD_BLOCKED_SEND && (caller->ipc_flags & IPC_CALL)));
    waitq_remove(caller);
    memset(&caller->msg, 0, sizeof(caller->msg));   /* delivered: P42 */
    caller->ipc_flags = 0;                           /* the offer stays (P59) */
    reply_link(server, caller, reply_flags);
}

void reply_complete(struct thread *server, int64_t rax)
{
    struct thread *caller = server->reply_to;

    KASSERT(server->reply_state == REPLY_PENDING && caller &&
            caller->reply_from == server && caller->state == THREAD_BLOCKED_REPLY);
    /* The wake unlinks both sides (it must still see REPLY_FAULT, P71),
     * leaving the authority marked gone; then it is consumed (P17). */
    thread_wake(caller, rax);
    KASSERT(server->reply_state == REPLY_CALLER_GONE && !server->reply_to);
    server->reply_state = REPLY_NONE;
}

void reply_consume_gone(struct thread *server)
{
    KASSERT(server->reply_state == REPLY_CALLER_GONE && !server->reply_to);
    server->reply_state = REPLY_NONE;
    server->reply_flags = 0;
}

void ipc_detach(struct thread *t, bool destroying)
{
    bool was_blocked = blocked(t->state);
    bool fault = fault_caller(t);

    /* Its own wait: a queue, or a reply it is waiting for. */
    waitq_remove(t);
    reply_unlink_caller(t);

    /* P28: authority it holds as a server dies with it (or with its
     * suspension): the caller is woken with T_EDEAD. */
    if (t->reply_state == REPLY_PENDING) {
        struct thread *caller = t->reply_to;
        KASSERT(caller && caller->reply_from == t && caller->state == THREAD_BLOCKED_REPLY);
        thread_wake(caller, T_EDEAD);
    }
    t->reply_to = NULL;
    t->reply_state = REPLY_NONE;
    t->reply_flags = 0;
    ipc_clear_staged(t);

    /* P30: an interrupted blocking syscall returns T_EINTR on resume. A
     * thread delivering a fault is not in a syscall: no register is
     * written, and on resume it re-executes the faulting instruction
     * (P72). */
    if (was_blocked && !destroying && !fault)
        t->regs.rax = (uint64_t)(int64_t)T_EINTR;
}

/* ---- audit (P9), test builds ---- */

#ifdef TARKA_SELFTEST

static struct thread *registry;

void thread_registry_add(struct thread *t)
{
    t->reg_prev = NULL;
    t->reg_next = registry;
    if (registry)
        registry->reg_prev = t;
    registry = t;
}

void thread_registry_remove(struct thread *t)
{
    if (t->reg_prev)
        t->reg_prev->reg_next = t->reg_next;
    else if (registry == t)
        registry = t->reg_next;
    if (t->reg_next)
        t->reg_next->reg_prev = t->reg_prev;
    t->reg_next = t->reg_prev = NULL;
}

static bool live(const struct thread *t)
{
    for (const struct thread *x = registry; x; x = x->reg_next)
        if (x == t)
            return true;
    return false;
}

/* A queue is a well-formed doubly linked list of live threads, each in
 * the queue's state and pointing back at it. */
static bool queue_ok(const struct waitq *q)
{
    const struct thread *prev = NULL;
    unsigned n = 0;

    if (!blocked(q->kind) || q->kind == THREAD_BLOCKED_REPLY)
        return false;
    if (!q->head != !q->tail)
        return false;
    for (const struct thread *t = q->head; t; prev = t, t = t->wait_next) {
        if (!live(t) || t->wait_q != q || t->wait_prev != prev || t->state != q->kind)
            return false;
        if (++n > 100000)
            return false;   /* a cycle */
    }
    return prev == q->tail;
}

static bool in_queue(const struct waitq *q, const struct thread *t)
{
    for (const struct thread *x = q->head; x; x = x->wait_next)
        if (x == t)
            return true;
    return false;
}

bool ipc_audit(struct waitq *const *extra, size_t nextra)
{
    const struct thread *cur = this_cpu()->current;

    for (size_t i = 0; i < nextra; i++)
        if (!queue_ok(extra[i]))
            return false;
    if (cur && !live(cur))
        return false;

    for (const struct thread *t = registry; t; t = t->reg_next) {
        if (t->state >= THREAD_STATE_COUNT)
            return false;
        /* 1 and 6: runnable iff queued to run; running iff current. */
        if ((t->state == THREAD_RUNNABLE) != runq_contains(t))
            return false;
        if ((t->state == THREAD_RUNNING) != (t == cur))
            return false;
        /* 2, 3, 5: in a wait queue iff blocked in one, and in that one. */
        bool queued_state = blocked(t->state) && t->state != THREAD_BLOCKED_REPLY;
        if (queued_state) {
            if (!t->wait_q || !queue_ok(t->wait_q) || !in_queue(t->wait_q, t))
                return false;
        } else if (t->wait_q || t->wait_next || t->wait_prev) {
            return false;
        }
        /* 4: waiting for a reply iff exactly one live server points back. */
        if ((t->state == THREAD_BLOCKED_REPLY) != (t->reply_from != NULL))
            return false;
        if (t->reply_from && (!live(t->reply_from) || t->reply_from->reply_to != t ||
                              t->reply_from->reply_state != REPLY_PENDING))
            return false;
        /* 7 (P42, P46): a message is staged only while blocked sending,
         * and nothing lies beyond its length. */
        unsigned len = t->state == THREAD_BLOCKED_SEND ? MSG_LEN(t->msg.info) : 0;
        if (len > MSG_MAX_WORDS || (t->state != THREAD_BLOCKED_SEND && t->msg.info))
            return false;
        for (unsigned i = len; i < MSG_MAX_WORDS; i++)
            if (t->msg.words[i])
                return false;
        unsigned nc = t->state == THREAD_BLOCKED_SEND ? MSG_NCAPS(t->msg.info) : 0;
        for (unsigned i = nc; i < MSG_MAX_CAPS; i++)
            if (t->msg.caps[i])
                return false;
        if (t->msg.reserved || (t->state != THREAD_BLOCKED_SEND && t->msg.badge))
            return false;
        /* 8 (P50, P59): an offer of receive slots is staged only while
         * receiving, waiting for a reply, or queued as a caller; nothing
         * lies beyond its count. */
        bool calling = t->state == THREAD_BLOCKED_SEND && (t->ipc_flags & IPC_CALL);
        bool may_offer = t->state == THREAD_BLOCKED_RECV ||
                         t->state == THREAD_BLOCKED_REPLY || calling;
        unsigned oc = may_offer ? t->offer.count : 0;
        if (oc > MSG_MAX_CAPS || (!may_offer && t->offer.count))
            return false;
        for (unsigned i = oc; i < MSG_MAX_CAPS; i++)
            if (t->offer.slots[i])
                return false;
        /* 9 (P59): flags only in the state they belong to. */
        uint32_t allowed = t->state == THREAD_BLOCKED_SEND ? (IPC_CALL | IPC_CALL_GRANT | IPC_FAULT)
                         : t->state == THREAD_BLOCKED_RECV ? IPC_REPLY_SKIPPED : 0;
        if ((t->ipc_flags & ~allowed) || ((t->ipc_flags & (IPC_CALL_GRANT | IPC_FAULT)) && !calling))
            return false;
        /* P76: a fault call never has an offer or call_grant, on either
         * side of the rendezvous. */
        if ((t->ipc_flags & IPC_FAULT) && ((t->ipc_flags & IPC_CALL_GRANT) || t->offer.count))
            return false;
        if ((t->reply_flags & REPLY_FAULT) &&
            ((t->reply_flags & REPLY_CALL_GRANT) || !t->reply_to || t->reply_to->offer.count))
            return false;
        /* 11 (P64): stopping is terminal for reply authority. */
        if (t->state == THREAD_INACTIVE && (t->reply_state != REPLY_NONE || t->reply_to))
            return false;
        /* 10 (P60): a receiver holds no reply authority; reply flags only
         * with a pending authority. */
        if (t->state == THREAD_BLOCKED_RECV && t->reply_state != REPLY_NONE)
            return false;
        if (t->reply_flags && t->reply_state != REPLY_PENDING)
            return false;
        /* Reply authority held as a server. */
        switch (t->reply_state) {
        case REPLY_PENDING:
            if (!t->reply_to || !live(t->reply_to) || t->reply_to->reply_from != t)
                return false;
            break;
        case REPLY_NONE:
        case REPLY_CALLER_GONE:
            if (t->reply_to)
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

#endif
