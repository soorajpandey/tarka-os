#ifndef TARKA_IPC_H
#define TARKA_IPC_H

#include <tarka/thread.h>

/*
 * IPC state machinery (ADR 0009): wait queues, reply links, wake-ups and
 * aborts. Every operation keeps the structural invariants of P9, which
 * ipc_audit() checks.
 */

/* Block the current, RUNNING thread `t` at the tail of `q`; its state
 * becomes q->kind. It leaves the CPU when the syscall returns. */
void waitq_block(struct thread *t, struct waitq *q);

struct thread *waitq_first(const struct waitq *q);

/* Wake a thread blocked in a queue or waiting for a reply: it leaves the
 * queue or reply link, gets `rax` as its syscall result, and becomes
 * RUNNABLE at the tail of the run queue (P10). A thread delivering a
 * fault is not in a syscall: it gets no result; T_OK resumes it and
 * anything else leaves it FAULTED (P70-P72). */
void thread_wake(struct thread *t, int64_t rax);

/* Wake every thread in `q` with `rax` (object death or authority loss,
 * P26, P27). */
#ifdef TARKA_SELFTEST
void waitq_wake_all(struct waitq *q, int64_t rax);   /* tests only: unbounded */
#endif

/* Link caller to server: the server gains reply authority, the caller is
 * BLOCKED_REPLY (P14, P16). The caller must not be on any queue. */
void reply_link(struct thread *server, struct thread *caller, uint32_t flags);

/* A call meets its receiver (P58): `caller` (RUNNING, or a queued caller
 * whose message has just been delivered) leaves its queue and its staged
 * message and flags, keeps its offer, and becomes BLOCKED_REPLY on
 * `server`, which gains reply authority. */
void ipc_call_link(struct thread *server, struct thread *caller, uint32_t reply_flags);

/* Commit a reply (P61): consume `server`'s PENDING authority and wake
 * its caller with `rax` (T_OK after a delivered reply). The reply must
 * already be delivered. For a fault caller, T_OK resumes it and anything
 * else leaves it FAULTED (P71). */
void reply_complete(struct thread *server, int64_t rax);

/* Consume an authority whose caller is gone (P29, P61). */
void reply_consume_gone(struct thread *server);

/* Leave every queue and reply link (P28-P31). If `t` was blocked and is
 * not being destroyed, its interrupted syscall will return T_EINTR. */
void ipc_detach(struct thread *t, bool destroying);

#ifdef TARKA_SELFTEST
void thread_registry_add(struct thread *t);
void thread_registry_remove(struct thread *t);
/* Every live thread object, linked by reg_next (tests enumerate binding
 * slots without following the CDT). */
const struct thread *thread_registry_first(void);
/* P9 over every live thread and every queue they reference, plus `extra`
 * queues (which may be empty or hold stale entries). */
bool ipc_audit(struct waitq *const *extra, size_t nextra);
#endif

#endif
