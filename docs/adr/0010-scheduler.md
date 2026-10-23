# ADR 0010: Scheduler, timer preemption and bounded kernel work

Status: proposed (step 6). Rule numbers SC1..SC24 are referenced from
code, tests and other ADRs.

## Context

Until now a thread runs until it yields, blocks, faults or exits. ADR 0008
T36 makes that a hard gate: no program outside the kernel's own test
suite may run until timer interrupts preempt user threads and a thread
that never yields cannot keep the CPU from others.

The scheduler is the first place where several invariants that so far
held locally become global: thread states, run-queue membership, the
current-thread pointer of each CPU, interrupt and kernel-entry state, and
the IPC wait queues and reply links. This ADR freezes that contract before
any timer code is written, in the same way ADR 0009 froze IPC.

What exists today, which this ADR keeps or replaces:
- one global FIFO run queue
- `current` is already per-CPU (`this_cpu()->current`)
- the kernel always runs with interrupts disabled (T10), and user code
  with them enabled
- an interrupt from user mode is acknowledged and the thread continues
- when nothing is runnable, the scheduler returns to a kernel
  continuation (the self-test driver); there is no idle loop
- the LAPIC timer works in one-shot mode but only one test arms it
- ADR 0005 already decided per-CPU run queues and a per-CPU idle context,
  and phase 1 SMP behind a big kernel lock

## Scope

In scope:
- the scheduling state machine and its audit
- per-CPU run queues, with only CPU 0 started
- round-robin with a fixed time quantum
- timer preemption of user threads
- the idle loop
- the kernel's non-preemption model and a bound on the work done in any
  one kernel entry, including the operations P37 names as unbounded
- the queue-depth bound of I-B1

Out of scope: starting other CPUs, migration and load balancing,
priorities, budgets and scheduling contexts (MCS), deadlines, timeouts on
IPC, and calibrating the quantum to wall-clock time.

## Thread ownership

**SC1. Every thread is owned by exactly one place, determined by its
state:**

| State | Owned by |
|---|---|
| RUNNING | the `current` pointer of exactly one CPU, its home CPU |
| RUNNABLE | exactly one run queue: its home CPU's |
| BLOCKED_SEND, BLOCKED_RECV, BLOCKED_NOTIF | exactly one wait queue (P9) |
| BLOCKED_REPLY | exactly one reply link (P9) |
| INACTIVE, FAULTED | nothing |

No thread is ever in two of these at once. In particular no thread is on
a run queue and in a wait queue or reply link, and no thread is both
`current` somewhere and queued anywhere.

**SC2. Home CPU.** Each thread has a home CPU, recorded in its TCB, set
when it is created (CPU 0 in this step) and never changed in this step.
A thread is enqueued only on its home CPU's run queue and runs only
there. Moving threads between CPUs is a later decision; when it comes, it
changes the home CPU explicitly, under the rules of ADR 0005.

**SC3. No global `current`.** Every use of "the current thread" names a
CPU (`this_cpu()->current`). The audit checks SC1 for every CPU, not just
the one it runs on. No code may assume there is only one CPU, even while
only one is started.

## Transitions

**SC4. One primitive per transition.** Thread state, run-queue membership
and each CPU's `current` change only through these functions, each of
which updates all three together and checks its precondition:

| Primitive | From | To | Effect |
|---|---|---|---|
| `sched_make_runnable(t)` | INACTIVE, FAULTED, any BLOCKED (already unlinked by IPC) | RUNNABLE | appended to the home run queue's tail |
| `sched_dispatch(cpu)` | RUNNABLE (head of the queue) | RUNNING | removed from the queue; becomes `cpu->current`; a fresh quantum is armed (SC10) |
| `sched_preempt(t)` | RUNNING | RUNNABLE | `current` cleared; appended to the tail |
| `sched_yield(t)` | RUNNING | RUNNABLE | the same as preemption, requested by the thread |
| block (`waitq_block`, `reply_link`) | RUNNING | BLOCKED_* | `current` cleared when the syscall returns to the scheduler |
| stop (`thread_stop`, `exit`) | any | INACTIVE or FAULTED | removed from whatever owned it (SC1), IPC detached (P64) |

The IPC primitives of ADR 0009 (`thread_wake`, `waitq_block`,
`ipc_call_link`, `reply_complete`, `ipc_detach`) keep their meaning and
call these, so no code outside this table writes `t->state` for a
scheduling transition. The existing direct writes in `sys_yield`,
`sys_exit` and `schedule()` go away.

**SC5. Atomicity.** The kernel is not preemptible (SC8). A transition
starts and finishes within one kernel entry with interrupts disabled, so
no observer, interrupt handler or other entry sees a thread half-moved.
The audit runs at the boundaries and checks SC1.

**SC6. Only stopped threads can be started.** `thread_resume` accepts
INACTIVE and FAULTED only (fixed in `041722c`). Preemption applies only
to the RUNNING thread; a queued or blocked thread can never be
preempted, because it is not running.

## Policy

**SC7. Round-robin, one level.** Each CPU's run queue is FIFO. A
dispatched thread runs until it blocks, yields, stops, faults or uses up
its quantum; a thread that yields or is preempted goes to the tail, and
so does a woken thread (P12). There are no priorities in this step.

## Preemption

**SC8. The kernel is not preemptible.** Kernel code runs with interrupts
disabled (T10, unchanged). There are exactly two places where interrupts
are enabled on a CPU:
1. user mode
2. the idle loop, across its `hlt` instruction only (SC13)

(Test builds also contain one deliberate `sti; nop; cli` window inside a
self-test, to take a pending interrupt in kernel mode on purpose. It is
test code, not a kernel path.)

A timer interrupt therefore never arrives in the middle of a syscall, an
IPC operation, a capability mutation or any other kernel code. If it
becomes pending while the kernel runs, it is taken immediately after the
return to user mode, or in the idle loop. **There is no kernel
preemption and no context switch from arbitrary kernel execution.** The
only places a CPU switches threads are `schedule()` after an entry
completes, and the idle loop.

**SC9. The preemption path:**

```
user code runs (IF=1)
  -> timer interrupt
  -> kernel entry: the T32 state, every register saved to the TCB (T6)
  -> EOI
  -> did this CPU's current quantum expire? (SC11)
       yes: sched_preempt(current)
       no:  continue the same thread
  -> schedule(): dispatch the head of the run queue (possibly the same
     thread, if it is alone)
  -> return to user mode through IRETQ (T18: interrupt entries never
     SYSRET)
```

A preempted thread's FPU and vector state is saved at entry (as for any
entry) and restored when it is next dispatched.

**SC10. The quantum.** On every dispatch the CPU arms its LAPIC timer in
one-shot mode for one quantum: a fixed number of timer ticks, `QUANTUM`.
The quantum is not reset by syscalls or other entries. Time the kernel
spends on a thread's behalf counts against that thread, because the
timer keeps counting while the kernel runs. A thread that blocks or yields
loses the rest of its quantum; the next thread gets a fresh one.
`QUANTUM` is a constant in timer ticks. Calibrating it to wall-clock time
is out of scope.

**SC11. Stale timer interrupts are not preemptions.** A timer interrupt
that became pending for an earlier arming (for example while the kernel
dispatched a different thread) must not preempt the new thread at once.
The interrupt counts as expiry only if this CPU's timer reads zero
(current-count register), meaning the arming made at the last dispatch has
run out. Otherwise it is acknowledged and ignored.

**SC12. Non-monopolisation (T36).** Let a CPU have N runnable threads,
and let W be the bound on the work of one kernel entry (SC16). A runnable
thread is dispatched again within `(N - 1) * (QUANTUM + W) + W` timer
ticks, whatever the other threads do: a dispatch lasts at most one
quantum plus one kernel entry, and the queue is FIFO. This is the
guarantee T36 requires, and the tests check it directly (SC22).

## Idle

**SC13. When nothing is runnable** on a CPU, it runs its idle loop on its
own kernel stack, with `current` NULL:

```
loop:
    if the run queue is not empty: dispatch
    sti; hlt; cli      (interrupts are enabled only across hlt)
    handle the interrupt that woke it (EOI, and anything it made runnable)
```

`sti; hlt` is atomic with respect to interrupts, so a wake-up can't be
lost between the check and the halt. The idle loop is not a thread: it
has no TCB, no capabilities and no user state, and nothing can block on
it, signal it or address it. The timer is not armed while idle.

Test builds keep a way to return to the self-test driver when the system
is idle, but it is reached through the idle loop. The idle path itself is
tested: halt, then wake on an interrupt.

## Multiple CPUs

**SC14. This step runs on one CPU, with per-CPU structures.**
- The run queue moves into the per-CPU data, next to `current`.
- The quantum timer is per-CPU (the LAPIC timer is per-CPU hardware).
- Cross-CPU wake-ups (enqueueing on another CPU's queue, plus an IPI)
  are not implemented. While only CPU 0 runs they cannot happen; SC2
  makes the home CPU explicit, so the first code that needs them is easy
  to find.
- Bringing up further CPUs, with the big kernel lock of ADR 0005, is a
  separate step with its own tests, including `-smp 4` in CI.

## Bounded kernel work

**SC15. Work model: restart by re-execution.** No thread keeps kernel
state across a scheduling boundary. There is one kernel stack per CPU,
every entry starts on it afresh, and `schedule()` never returns to the
code that called it. So "a syscall stays in progress inside the kernel"
is not a model this kernel has, and none is added. Long operations work
like this instead:
- An operation does a bounded chunk of work. Each chunk ends in a state
  where every invariant holds; the progress made is committed and
  recorded in kernel objects (the CDT, the queues, zombie capabilities),
  never in a hidden continuation in the TCB or on a stack.
- If work remains, the syscall is **restarted**: the thread is made
  RUNNABLE at the tail of the run queue (a preemption point, SC7) with its
  registers set so that it re-executes the same SYSCALL instruction (RIP
  back by 2, RAX set back to the syscall number). Its arguments are
  untouched, because a restarted syscall writes no result registers.
- When it runs again it re-enters the syscall from the start, sees the
  progress already made, and continues.
- Every chunk is valid committed state. A thread suspended or destroyed
  between chunks leaves a consistent system, and whoever repeats the
  operation continues it.

The user never sees a partial result from a restarted syscall: `recv` or
`delete` either completes or hasn't returned yet. Revocation keeps its
existing, user-visible budget (`T_EAGAIN`), because that is its API, but
the kernel caps the budget (SC17).

**SC16. W, the per-entry work bound.** Every kernel entry does at most a
constant amount of work, W. The constants are named in the code:
- `WAKE_CHUNK` waiters woken
- `DRAIN_CHUNK` senders failed
- `REVOKE_CHUNK` capabilities deleted
- `ZERO_CHUNK` bytes zeroed
- `CNODE_CHUNK` slots drained

The inventory below lists every kernel operation whose work is not
already constant.

### Inventory of unbounded work, and its treatment

| Operation | Today | Treatment |
|---|---|---|
| Wake-all on endpoint/notification destruction or authority loss (P26, P27, P37 #1) | O(waiters) in one entry | **SC18**: wake at most `WAKE_CHUNK`; the deleting syscall restarts until none remain |
| `recv`/`reply_recv` drain of undeliverable senders (P53, P37 #2) | O(failed senders) in one entry | **SC19**: fail at most `DRAIN_CHUNK`, then restart the receive |
| `cap_revoke` with budget 0 ("no limit") | the whole subtree in one entry | **SC17**: the budget is capped at `REVOKE_CHUNK` |
| Deleting a CNode tree (ADR 0006, pending list) | the whole tree in one entry | **SC20**: drain at most `CNODE_CHUNK` slots per entry, the CNode staying a zombie meanwhile |
| Retype zeroing (O6) | up to 1 GiB in one entry | **SC21**: zero at most `ZERO_CHUNK` per entry; the new capabilities appear only once the zeroing is complete |
| Computing a semantic count (P27a, `count_with`) | O(copies of that object) per operation | **decision needed, below** |
| Audit and `runq_contains` | O(threads) | test builds only; not part of W |

**SC17. Revocation budget.** `cap_revoke` treats a budget of 0, or any
budget above `REVOKE_CHUNK`, as `REVOKE_CHUNK`. The API is unchanged:
`T_EAGAIN` means "call again".

**SC18. Wake-all in chunks.** When deleting a capability (or an object)
requires waking waiters (P26, P27, P43, P44), at most `WAKE_CHUNK` are
woken per entry:
- If waiters remain, **the capability is not deleted yet**. It stays in
  its slot, marked as a zombie that grants nothing, and the deleting
  syscall restarts (SC15).
- The rules stay exactly as they are, because they are evaluated on
  committed state. While the zombie exists the authority still counts as
  present (it is not yet deleted), so the remaining waiters are
  legitimately still waiting. The last chunk deletes the capability and
  wakes the last waiters together.
- A zombie can't be used for anything but completing its own deletion:
  every lookup treats it as empty for the purpose of authority, and as
  occupied for the purpose of placing a new capability.

**SC19. Receive drain in chunks.** A receive (`recv`, or `reply_recv`
before its reply commits) fails at most `DRAIN_CHUNK` undeliverable
senders per entry. If the head is still undeliverable after that, the
receive restarts (SC15). Each failed sender is still failed exactly once
(I-E2), and the receive's result, when it comes, is exactly what it would
have been in one pass. For `reply_recv`, the reply half is not committed
until the drain is done (P63 phase 3 comes before phase 5), so a
restarted `reply_recv` re-validates both halves from the start.

**SC20. CNode deletion in chunks.** A CNode whose last capability is
deleted drains at most `CNODE_CHUNK` slots per entry. Until it is empty,
its capability remains as a zombie (SC18) and the deleting syscall
restarts. This retires the known limitation in ADR 0006.

**SC21. Retype zeroing in chunks.** Retype advances a zeroing watermark
inside the Untyped by at most `ZERO_CHUNK` bytes per entry and restarts
until the whole span is zero. The new capabilities are installed only in
the final chunk, so no object is ever visible before it is fully zeroed
(O6). The Untyped's watermark stays in place throughout, so no other
retype can claim the same memory.

### Decision needed: semantic counts

Today P27a counts are computed by walking the object's run of capability
records. That is O(copies of the object) per `send`, `recv`, `wait` and
deletion. The copies are paid for by whoever made them, but the cost is
not bounded by a constant, so it breaks SC16. There are two options:
- **(a) Maintained counters:** two per-object counters (READ and WRITE
  holders), updated in the same step as every capability creation,
  deletion and rights change. The audit already rescans every capability
  table independently (P27a tests) and would check the counters against
  the rescan after every step, so a stale counter is caught.
- **(b) Keep computing,** and document the bound as "O(copies)". That
  breaks SC16 as written.

Step 5 deliberately avoided cached counts ("don't optimize"). Step 6 is
where they become necessary for a hard bound. I recommend **(a)**,
verified by the existing independent rescan, and would like you to
confirm it.

## Queue depth (closes I-B1)

**SC22. Every queue holds each live thread at most once.** Every wait
queue and every run queue contains at most one entry per live thread,
because each thread has one wait link and one run link and SC1 lets it use
at most one of them. The audit checks this mechanically, replacing the
current fixed loop limit:
- each queue's length is at most the number of live threads
- no thread appears twice in any queue
- no thread is in two queues
- links are symmetric and the walk has no cycle

## Invariants introduced

| ID | Invariant | Rules |
|---|---|---|
| I-SC1 | Every thread is owned by exactly the place its state names; never by two | SC1-SC4 |
| I-SC2 | No context switch happens from arbitrary kernel execution; interrupts are enabled only in user mode and across the idle `hlt` | SC8, SC13 |
| I-SC3 | A runnable thread is dispatched within `(N-1)(QUANTUM+W)+W` ticks (T36) | SC10-SC12 |
| I-SC4 | Every kernel entry does at most W work; long operations restart and commit only valid states | SC15-SC21 |
| I-B1 (closed) | IPC work, message size and queue depth are bounded | P37, SC16, SC22 |

## Tests required

1. **T36, non-monopolisation:** two (and five) threads that spin forever
   without a syscall. All make progress. The dispatch order is
   round-robin, and every thread's wait between dispatches is within the
   SC12 bound, measured in timer ticks.
2. **Preemption state:** a preempted thread is RUNNABLE at the tail with
   every register intact (the preserve-style register checker runs under
   preemption); it returns through IRETQ; FPU and vector state survive.
3. **A timer that fires while the kernel runs** is taken after the return
   to user mode, never inside the kernel (an entry-state check counts
   interrupts with a kernel-mode CS: zero, apart from idle).
4. **Stale timer interrupt** (SC11): a forced pending timer interrupt at
   dispatch does not preempt the new thread.
5. **Idle:** the CPU halts with nothing runnable and wakes on an
   interrupt; the idle loop has no TCB, and `current` is NULL while
   idle.
6. **IPC under preemption:** the ADR 0009 suites rerun with preemption
   enabled and a short quantum; the audit holds after every step. A
   thread is never preempted while queued.
7. **Restart (SC15):**
   - `recv` with more than `DRAIN_CHUNK` undeliverable senders: the
     result is identical to the one-pass result, and each sender is
     failed once
   - deleting an endpoint with more than `WAKE_CHUNK` waiters
   - deleting a large CNode tree
   - retype of a large span: nothing visible before the end
   - in each, the thread is suspended between chunks and the system is
     consistent; resuming continues the operation
8. **Bounded entries:** the worst-case kernel entry, measured in the
   longest chunked operations, stays within W; stack high-water stays
   within budget (T33).
9. **Queue bounds** (SC22): audit checks on queues of 0, 1 and many
   threads.

### Planned mutants

- duplicate insertion into a run queue or a wait queue
- a thread in two queues (run queue and a wait queue)
- a stale link left behind after removal
- a queue cycle
- queue length exceeding the number of live threads
- removing the wrong thread (neighbour instead of target)
- preempting a thread that is queued or blocked
- waking a thread without enqueueing it
- enqueueing a thread without changing its state
- dispatching without setting `current`, or leaving `current` set when
  blocking
- a stale timer interrupt treated as expiry
- the quantum not re-armed on dispatch
- preemption returning through SYSRET
- interrupts enabled in kernel code outside the idle `hlt`
- a chunked operation committing an invalid intermediate state, such as
  a zombie that still grants authority, or an object visible before it
  is zeroed
- a restarted syscall that writes result registers, or doesn't rewind
  RIP/RAX
- the drain failing a sender twice across a restart
- a revoke budget of 0 left unlimited
- maintained counters (if chosen) not updated on derive, delete or a
  rights change

## Implementation order

1. Ownership and transitions: move the run queue into per-CPU data, add
   the home CPU, route every state change through the SC4 primitives,
   and extend the audit (SC1, SC22). There is no timer yet, so behaviour
   is unchanged and every existing test must pass.
2. The idle loop (SC13).
3. Timer preemption and the quantum (SC8-SC12), with the T36 tests.
4. Restart by re-execution (SC15) and the chunked operations, one at a
   time: revocation budget, receive drain, wake-all with zombies, CNode
   drain, retype zeroing. If (a) is chosen, maintained counters come
   before the chunking.
5. Mutation testing, review, documentation. T36 is then satisfied.
