/*
 * Thread scheduling state (ADR 0010 SC1-SC8, SC29; revision 3f). See
 * <tarka/sched.h>. The only writer of t->state, run-queue links and
 * `current` (gate: scripts/check-sched.sh).
 */

#include <tarka/sched.h>

#include <tarka/cont.h>
#include <tarka/errno.h>
#include <tarka/ipc.h>
#include <tarka/kernel.h>
#include <tarka/reaper.h>
#include <tarka/string.h>
#include <tarka/vspace.h>

#include "../arch/x86_64/lapic.h"
#include "../arch/x86_64/percpu.h"

/* Per-CPU scheduling data: the run queue (FIFO, intrusive through the
 * TCB's rq links) and the idle thread. */
#ifdef TARKA_SELFTEST
uint32_t sched_boot_timer_count = UINT32_MAX;
const struct thread *sched_trace[SCHED_TRACE_MAX];
unsigned sched_trace_n;
#endif

struct sched_cpu {
    struct thread *rq_head, *rq_tail;
    uint32_t       rq_len;
    uint32_t       reserved;
    struct sched_timer timer;   /* A2: the quantum timer's owner */
#ifdef TARKA_SELFTEST
    uint32_t       test_wake;   /* ticks for the idle thread's next halt */
    uint32_t       test_reserved;
#endif
    struct thread  idle;   /* SC5: kernel-owned, never addressable */
};

static struct sched_cpu cpus[SCHED_MAX_CPUS];

static struct sched_cpu *cpu_of(uint32_t id)
{
    KASSERT(id < SCHED_MAX_CPUS);
    return &cpus[id];
}

static struct sched_cpu *this_sched(void)
{
    return cpu_of(this_cpu()->cpu_id);
}

static bool blocked(uint32_t s)
{
    return s == THREAD_BLOCKED_SEND || s == THREAD_BLOCKED_RECV ||
           s == THREAD_BLOCKED_REPLY || s == THREAD_BLOCKED_NOTIF;
}

/* ---- the run queue: private, so membership cannot change except with
 * the state (G11) ---- */

static bool on_runq(const struct thread *t)
{
    return t->rq_prev || t->rq_next || cpu_of(t->home_cpu)->rq_head == t;
}

static void rq_push(struct thread *t)
{
    struct sched_cpu *c = cpu_of(t->home_cpu);   /* SC3: the home queue */

    KASSERT(!on_runq(t));
    t->rq_next = NULL;
    t->rq_prev = c->rq_tail;
    if (c->rq_tail)
        c->rq_tail->rq_next = t;
    else
        c->rq_head = t;
    c->rq_tail = t;
    c->rq_len++;
}

static void rq_remove(struct thread *t)
{
    struct sched_cpu *c = cpu_of(t->home_cpu);

    if (!on_runq(t))
        return;
    if (t->rq_prev)
        t->rq_prev->rq_next = t->rq_next;
    else
        c->rq_head = t->rq_next;
    if (t->rq_next)
        t->rq_next->rq_prev = t->rq_prev;
    else
        c->rq_tail = t->rq_prev;
    t->rq_next = t->rq_prev = NULL;
    KASSERT(c->rq_len > 0);
    c->rq_len--;
}

/* ---- boot ---- */

void sched_boot(void)
{
    struct sched_cpu *c = this_sched();

    /* G3: the idle thread is zero but for its state and home: no
     * binding, no continuation, no reply authority, zero registers. */
    memset(&c->idle, 0, sizeof(c->idle));
    c->idle.state = THREAD_IDLE;
    c->idle.home_cpu = this_cpu()->cpu_id;
    this_cpu()->current = &c->idle;   /* SC2: never NULL from here on */
    /* H1: the scheduler owns the timer from boot: set up, stopped, and no
     * quantum running. */
    memset(&c->timer, 0, sizeof(c->timer));
    if (lapic_present()) {
        lapic_timer_setup();
#ifdef TARKA_SELFTEST
        sched_boot_timer_count = lapic_timer_count();
#endif
    }
}

void sched_thread_init(struct thread *t)
{
    KASSERT(t->state == THREAD_INACTIVE);
    t->home_cpu = 0;   /* SC3: CPU 0 in this step; never written again */
}

struct thread *sched_idle(void)
{
    return &this_sched()->idle;
}

struct thread *sched_current(void)
{
    return this_cpu()->current;
}

bool runq_empty(void)
{
    return this_sched()->rq_head == NULL;
}

/* ---- the transitions (G1), each committing its pair (G11) ---- */

int sched_start(struct thread *t)
{
    /* SC8: only a stopped thread, and only with what it needs to run (T4).
     * A destroyed thread has no bindings left, so it is refused too. */
    if ((t->state != THREAD_INACTIVE && t->state != THREAD_FAULTED) || !thread_runnable_safe(t))
        return T_EINVAL;
    KASSERT(!on_runq(t) && !t->wait_q && !t->reply_from && t->cont.kind == CONT_NONE);
    t->state = THREAD_RUNNABLE;
    rq_push(t);
    return T_OK;
}

int sched_wake(struct thread *t)
{
    /* IPC has unlinked it from its wait queue or reply link already. */
    if (!blocked(t->state) || t->wait_q || t->reply_from)
        return T_EINVAL;
    KASSERT(!on_runq(t));
    t->state = THREAD_RUNNABLE;
    rq_push(t);
    return T_OK;
}

int sched_yield(struct thread *t)
{
    if (t->state != THREAD_RUNNING || this_cpu()->current != t)
        return T_EINVAL;
    t->state = THREAD_RUNNABLE;
    rq_push(t);   /* to the tail: a continuation gets no priority (G6) */
    return T_OK;
}

int sched_block(struct thread *t, uint32_t kind)
{
    /* IPC has linked it into the wait queue of that kind (P9). */
    if (t->state != THREAD_RUNNING || this_cpu()->current != t || !t->wait_q ||
        (kind != THREAD_BLOCKED_SEND && kind != THREAD_BLOCKED_RECV &&
         kind != THREAD_BLOCKED_NOTIF) || t->wait_q->kind != kind)
        return T_EINVAL;
    KASSERT(!on_runq(t));
    t->state = kind;
    return T_OK;
}

int sched_block_reply(struct thread *t)
{
    /* IPC has made the reply link and taken it out of any queue (P58). */
    if ((t->state != THREAD_RUNNING && t->state != THREAD_BLOCKED_SEND) || t->wait_q ||
        !t->reply_from)
        return T_EINVAL;
    KASSERT(!on_runq(t));
    t->state = THREAD_BLOCKED_REPLY;
    return T_OK;
}

int sched_fault(struct thread *t)
{
    if ((t->state != THREAD_RUNNING && t->state != THREAD_BLOCKED_SEND &&
         t->state != THREAD_BLOCKED_REPLY) || t->wait_q || t->reply_from)
        return T_EINVAL;
    KASSERT(!on_runq(t) && t->cont.kind == CONT_NONE);
    t->state = THREAD_FAULTED;
    return T_OK;
}

#ifdef TARKA_SELFTEST
const struct thread *sched_stop_watch;
uint64_t sched_stop_watch_units = UINT64_MAX;
#endif

int sched_stop(struct thread *t, bool destroying)
{
    if (t->state == THREAD_IDLE || t->state >= THREAD_STATE_COUNT)
        return T_EINVAL;
#ifdef TARKA_SELFTEST
    if (t == sched_stop_watch)
        sched_stop_watch_units = reaper_units_total;
#endif
    /* The one funnel (F6, E1, P28-P31): what the continuation holds, then
     * every IPC queue and reply link, then the run queue and the state.
     * A thread stopped in a blocking syscall gets T_EINTR (unless it is
     * being destroyed). */
    cont_abandon(t, destroying);
    ipc_detach(t, destroying);
    rq_remove(t);
    t->state = THREAD_INACTIVE;
    /* Running here: leave its address space now, before whatever is
     * being deleted can be freed. */
    if (this_cpu()->current == t)
        vspace_activate_kernel();
    return T_OK;
}

#ifdef TARKA_SELFTEST
uint64_t sched_handoffs;
#endif

void sched_release_current(struct thread *t)
{
    KASSERT(t->state == THREAD_INACTIVE);
    if (this_cpu()->current == t) {
        this_cpu()->current = &this_sched()->idle;
        vspace_activate_kernel();
#ifdef TARKA_SELFTEST
        sched_handoffs++;
#endif
    }
}

struct thread *sched_dispatch(void)
{
    struct sched_cpu *c = this_sched();
    struct thread *t = c->rq_head;
    struct thread *cur = this_cpu()->current;

    /* At a boundary the outgoing thread has already left RUNNING. */
    KASSERT(cur && cur->state != THREAD_RUNNING);
    if (!t) {
        this_cpu()->current = &c->idle;   /* SC5: only when the queue is empty */
        vspace_activate_kernel();
        return &c->idle;
    }
    KASSERT(t->state == THREAD_RUNNABLE && t->home_cpu == this_cpu()->cpu_id);
    rq_remove(t);
    t->state = THREAD_RUNNING;
    this_cpu()->current = t;
    c->timer.dispatch_gen++;   /* A2: counts non-idle dispatches only */
#ifdef TARKA_SELFTEST
    if (sched_trace_n < SCHED_TRACE_MAX)
        sched_trace[sched_trace_n] = t;
    sched_trace_n++;
#endif
    return t;
}

#ifdef TARKA_SELFTEST
bool sched_wake_armed;
uint64_t sched_idle_halts, sched_idle_wakes;
uint64_t sched_idle_reap_passes;
unsigned sched_reap_max_pass;

bool runq_contains(const struct thread *t)
{
    for (const struct thread *x = this_sched()->rq_head; x; x = x->rq_next)
        if (x == t)
            return true;
    return false;
}

const struct thread *runq_first(void)
{
    return this_sched()->rq_head;
}

void sched_timer_get(struct sched_timer *out)
{
    *out = this_sched()->timer;
}

struct sched_timer *sched_timer_raw(void)
{
    return &this_sched()->timer;
}

/* The hardware is a quantum's while one runs: the tests may not touch it. */
int sched_test_timer_arm(uint32_t count)
{
    if (this_sched()->timer.armed)
        return T_EBUSY;
    if (count == 0)
        return T_EINVAL;
    lapic_timer_arm(count);
    return T_OK;
}

int sched_test_timer_stop(void)
{
    if (this_sched()->timer.armed)
        return T_EBUSY;
    lapic_timer_stop();
    return T_OK;
}

void sched_test_wake(uint32_t ticks)
{
    KASSERT(ticks > 0);
    this_sched()->test_wake = ticks;
    sched_wake_armed = true;
}

void sched_idle_arm_test_wake(void)
{
    struct sched_cpu *c = this_sched();

    KASSERT(sched_current() == &c->idle && !c->timer.armed);
    if (c->test_wake) {
        lapic_timer_arm(c->test_wake);
        c->test_wake = 0;
    }
}

const struct thread *runq_last(void)
{
    return this_sched()->rq_tail;
}

uint32_t runq_len(void)
{
    return this_sched()->rq_len;
}
#endif
