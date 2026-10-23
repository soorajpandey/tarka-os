#ifndef TARKA_SCHED_H
#define TARKA_SCHED_H

#include <tarka/thread.h>

/*
 * Thread scheduling state (ADR 0010 SC1-SC8, SC29; revision 3f G1-G11).
 *
 * This module is the only writer of a thread's state, of run-queue
 * membership and of each CPU's `current` (G2; scripts/check-sched.sh).
 * Each legal edge of the state machine has exactly one primitive, which
 * commits the state together with the scheduler-owned membership (G11).
 * A primitive called from any other state returns T_EINVAL and changes
 * nothing (G9); kernel callers assert success. Every primitive is O(1)
 * in the scheduler's own work (G5).
 *
 *   INACTIVE, FAULTED        -> RUNNABLE        sched_start   (resume, SC8)
 *   BLOCKED_* (unlinked)     -> RUNNABLE        sched_wake    (IPC wake)
 *   RUNNABLE (queue head)    -> RUNNING         sched_dispatch
 *   RUNNING                  -> RUNNABLE        sched_yield
 *   RUNNING (linked by IPC)  -> BLOCKED_SEND/RECV/NOTIF   sched_block
 *   RUNNING, BLOCKED_SEND    -> BLOCKED_REPLY   sched_block_reply
 *   RUNNING, BLOCKED_SEND,
 *   BLOCKED_REPLY (unlinked) -> FAULTED         sched_fault
 *   any but IDLE             -> INACTIVE        sched_stop    (the one funnel)
 *   boot                     -> IDLE            sched_boot    (idle threads)
 *
 * No migration (SC3, G7): a thread's home CPU is fixed when it is created.
 */

#define SCHED_MAX_CPUS 1

void sched_boot(void);
void sched_thread_init(struct thread *t);   /* a new thread object: home CPU */

__must_check int sched_start(struct thread *t);
__must_check int sched_wake(struct thread *t);
__must_check int sched_yield(struct thread *t);
__must_check int sched_block(struct thread *t, uint32_t kind);
__must_check int sched_block_reply(struct thread *t);
__must_check int sched_fault(struct thread *t);
__must_check int sched_stop(struct thread *t, bool destroying);

/* At a scheduling boundary: the head of this CPU's run queue becomes
 * RUNNING and `current`; with an empty queue, the idle thread does. */
struct thread *sched_dispatch(void);

/* G3: a thread destroying itself hands `current` to the idle thread
 * before anything of it is torn down. */
void sched_release_current(struct thread *t);

/* This CPU's idle thread, and this CPU's current thread (never NULL
 * after sched_boot, SC2). */
struct thread *sched_idle(void);
struct thread *sched_current(void);

bool runq_empty(void);

/* Per-CPU quantum timer state (ADR 0010 revision 3g A2), kept by sched.c
 * alone. `armed` is authoritative: the hardware count reads 0 both when a
 * quantum ran out and when the timer is stopped. A quantum's owner is the
 * thread together with the generation of the dispatch that armed it. */
struct sched_timer {
    uint32_t             armed;          /* a quantum is running */
    uint32_t             reserved;
    const struct thread *owner;          /* NULL unless armed */
    uint64_t             owner_gen;      /* 0 unless armed */
    uint64_t             dispatch_gen;   /* non-idle dispatches; never reset */
};

#ifdef TARKA_SELFTEST
bool runq_contains(const struct thread *t);
const struct thread *runq_first(void);     /* read-only walk via rq_next */
const struct thread *runq_last(void);
uint32_t runq_len(void);
/* G4: a wake source the tests armed. While set, the idle thread halts
 * (and must be woken) instead of returning to the test driver. */
extern bool sched_wake_armed;
/* Idle passes that did reaper work, and the most units in one pass. */
extern uint64_t sched_idle_reap_passes;
extern unsigned sched_reap_max_pass;
/* Threads dispatched, in order (the first SCHED_TRACE_MAX; the count
 * keeps going). */
#define SCHED_TRACE_MAX 256
extern const struct thread *sched_trace[SCHED_TRACE_MAX];
extern unsigned sched_trace_n;
/* `current` handed to the idle thread by sched_release_current(). */
extern uint64_t sched_handoffs;
/* The timer state, read-only copy and (for corruption tests) the raw one. */
void sched_timer_get(struct sched_timer *out);
struct sched_timer *sched_timer_raw(void);
/* H1: tests program the timer only through the scheduler, and only while
 * no quantum is running. */
__must_check int sched_test_timer_arm(uint32_t count);   /* T_EBUSY, T_EINVAL */
__must_check int sched_test_timer_stop(void);           /* T_EBUSY */
/* The count read immediately after the boot-time setup (H1: stopped). */
extern uint32_t sched_boot_timer_count;
/* G4, H1: a timer wake for the idle thread. It is armed by the idle
 * thread itself, immediately before it halts. */
void sched_test_wake(uint32_t ticks);
void sched_idle_arm_test_wake(void);
/* The instruction after the idle loop's HLT (G4). */
extern const char idle_hlt_resume[];
/* The reaper's unit count when sched_stop() stopped `sched_stop_watch`
 * (UINT64_MAX until it does). */
extern const struct thread *sched_stop_watch;
extern uint64_t sched_stop_watch_units;
extern uint64_t sched_idle_halts, sched_idle_wakes;
#endif

#endif
