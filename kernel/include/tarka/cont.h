#ifndef TARKA_CONT_H
#define TARKA_CONT_H

#include <tarka/thread.h>

/*
 * Thread continuations (ADR 0010 SC16, revision 3e E0-E5).
 *
 * A long operation runs as bounded chunks. The first runs in the syscall;
 * if the operation is not finished, the thread keeps a continuation, is
 * RUNNABLE, and goes to the tail of the run queue. Each later dispatch
 * runs one chunk instead of entering user mode. The final chunk writes
 * the syscall's result. A continuation exists only on a RUNNABLE or
 * RUNNING thread (E4).
 */

/* Dispatch: run `t`'s next chunk. `t` is RUNNING with a continuation.
 * On return it is RUNNING with the result written (enter user mode), or
 * RUNNABLE and queued (another chunk later), or blocked or stopped. */
void cont_run(struct thread *t);

/* After a first chunk in a syscall: keep the continuation if `rc` is
 * CONT_PENDING (the thread yields: RUNNABLE, queued, no result written:
 * SYS_NORESULT), otherwise clear it and pass `rc` through. */
int64_t cont_after_first(struct thread *t, int64_t rc);

/* The abandonment funnel (F6, E1): every stop of `t` comes here first.
 * Releases what the continuation holds, discards it, and unless the
 * thread is being destroyed leaves T_EINTR as the syscall's result. */
void cont_abandon(struct thread *t, bool destroying);

#ifdef TARKA_SELFTEST
extern uint64_t cont_chunks_run;
#endif

#endif
