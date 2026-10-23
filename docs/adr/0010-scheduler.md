# ADR 0010: Scheduler, timer preemption and bounded kernel work

Status: proposed, revision 2 (step 6). Rule numbers SC1..SC30 are
referenced from code, tests and other ADRs.

Revision 2 resolves the review of revision 1 (`5502c54`):
- the idle mechanism now follows ADR 0005 (a per-CPU idle thread)
- restarting a syscall by re-executing it is replaced by explicit
  continuations
- maintained semantic counters are adopted
- W is defined, and every operation has a named bound
- the fairness bound, stale-timer detection, the no-migration rule and the
  lifecycle of zombies are specified

Reading the code for the inventory also found four more unbounded paths
in the capability layer. They are covered in "Bounded kernel work".

## Context

Until now a thread runs until it yields, blocks, faults or exits. ADR 0008
T36 makes that a hard gate: no program outside the kernel's own test
suite may run until timer interrupts preempt user threads and a thread
that never yields cannot keep the CPU from others.

The scheduler is the first place where several invariants that so far
held locally become global: thread states, run-queue membership, the
current-thread pointer of each CPU, interrupt and kernel-entry state, and
the IPC wait queues and reply links. A preemptive scheduler is worthless
if a single syscall can still run for an unbounded time, so this ADR also
bounds the work of every kernel entry.

What exists today:
- one global FIFO run queue
- `current` is per-CPU (`this_cpu()->current`) and NULL when nothing runs
- the kernel always runs with interrupts disabled (T10), user code with
  them enabled
- an interrupt from user mode is acknowledged and the thread continues
- when nothing is runnable, the scheduler returns to a kernel
  continuation (the self-test driver); there is no idle thread yet
- the LAPIC timer works in one-shot mode, armed only by one test
- ADR 0005 decided, for every CPU: a kernel stack, an **idle thread** and
  a scheduler run queue; and SMP phase 1 behind a big kernel lock

## Scope

In scope:
- the scheduling state machine and its audit
- per-CPU run queues and idle threads, with only CPU 0 started
- round-robin with a fixed quantum, and timer preemption of user threads
- the kernel's non-preemption model
- a bound W on the work between two scheduling boundaries, including the
  operations P37 names as unbounded and every other unbounded path found
- maintained semantic capability counters
- the I-B1 queue-depth bound

Out of scope:
- starting other CPUs
- migration and load balancing
- priorities, budgets and scheduling contexts (MCS), deadlines
- IPC timeouts
- calibrating the quantum to wall-clock time

## Thread ownership

**SC1. Every thread is owned by exactly one place, determined by its
state:**

| State | Owned by |
|---|---|
| RUNNING | the `current` pointer of its home CPU |
| RUNNABLE | its home CPU's run queue, exactly once |
| BLOCKED_SEND, BLOCKED_RECV, BLOCKED_NOTIF | exactly one wait queue (P9) |
| BLOCKED_REPLY | exactly one reply link (P9) |
| INACTIVE, FAULTED | nothing |
| IDLE (idle threads only) | the `current` pointer of its CPU, when no other thread is RUNNING there |

No thread is ever owned by two places. In particular:
- no thread is on a run queue and in a wait queue or reply link
- no thread is `current` on a CPU and queued anywhere

**SC2. `current` is never NULL** once a CPU has started scheduling. It is
either a RUNNING user thread or that CPU's idle thread.

**SC3. No migration in step 6.** Every thread has a home CPU, recorded in
its TCB and set when it is created (CPU 0 in this step). It never
changes. Invariant: **`t->home_cpu` is the CPU whose run queue or
`current` owns `t`** whenever it is RUNNABLE or RUNNING. Migration and
load balancing will need their own ADR.

**SC4. No global `current`.** Every use of "the current thread" names a
CPU. The audit checks SC1 and SC2 for every CPU, not only the one it
runs on.

## The idle thread

**SC5. Each CPU has one idle thread** (ADR 0005). It is a kernel-owned
TCB, statically allocated, one per CPU:
- It is not made from an Untyped. No capability can name it, so no
  syscall, IPC operation or other thread can affect it.
- It runs in ring 0 on its CPU's kernel stack with `current` pointing at
  it:

```
loop:
    sti; hlt; cli      interrupts are enabled only across hlt
    handle the interrupt that woke it
    if the run queue is not empty: dispatch (SC7)
```

  `sti; hlt` is atomic with respect to interrupts, so a wake-up between
  the check and the halt cannot be lost.
- Its state is IDLE, a state no other thread can have. It is never
  RUNNABLE, never in a queue, never blocked, suspended or destroyed, and
  it holds no reply authority. It is dispatched only when the run queue
  is empty, and it gives up the CPU as soon as the queue is not.
- The quantum timer is not armed while it runs.

Test builds may leave the idle thread for the self-test driver once the
system is idle, but only through the idle thread itself, so the halt-and-
wake path is always exercised.

## Transitions

**SC6. One primitive per transition.** Thread state, run-queue membership
and each CPU's `current` change only through these functions, each
updating all three together and asserting its precondition:

| Primitive | From | To | Effect |
|---|---|---|---|
| `sched_make_runnable(t)` | INACTIVE, FAULTED, or BLOCKED (already unlinked by IPC) | RUNNABLE | appended to the home run queue |
| `sched_dispatch(cpu)` | RUNNABLE (queue head), or the idle thread when the queue is empty | RUNNING (or IDLE) | removed from the queue; becomes `cpu->current`; for a user thread a fresh quantum is armed (SC11) |
| `sched_preempt(t)` | RUNNING | RUNNABLE | appended to the tail; `current` passes to the next dispatch |
| `sched_yield(t)` | RUNNING | RUNNABLE | as preemption, requested by the thread |
| block (`waitq_block`, `reply_link`) | RUNNING | BLOCKED_* | `current` passes to the next dispatch |
| stop (`thread_stop`, `exit`) | any but IDLE | INACTIVE or FAULTED | removed from whatever owned it; IPC detached (P64) |

The IPC primitives of ADR 0009 (`thread_wake`, `waitq_block`,
`ipc_call_link`, `reply_complete`, `ipc_detach`) call these. No other code
writes `t->state` for a scheduling transition; the existing direct writes
in `sys_yield`, `sys_exit` and `schedule()` are removed.

**SC7. Atomicity.** The kernel is not preemptible (SC9). Every transition
starts and finishes in one kernel entry with interrupts disabled, so no
interrupt handler or other entry sees a thread half-moved. The audit runs
at scheduling boundaries (SC9) and checks SC1-SC4.

**SC8. Only stopped threads can be started.** `thread_resume` accepts
INACTIVE and FAULTED only (`041722c`). Preemption applies only to the
RUNNING thread of the CPU taking the interrupt.

```
INACTIVE  --resume--> RUNNABLE
FAULTED   --resume--> RUNNABLE
BLOCKED_* --resume--> refused (T_EBUSY)
RUNNING   --resume--> refused (T_EBUSY)
RUNNABLE  --resume--> refused (T_EBUSY)
IDLE      -- not addressable
```

## Preemption

**SC9. The kernel is not preemptible.** Kernel code runs with interrupts
disabled (T10, unchanged). Interrupts are enabled only:
1. in user mode
2. in the idle thread, across `hlt`

(Test builds also contain one deliberate `sti; nop; cli` window inside a
self-test, to take a pending interrupt in kernel mode. It is test code,
not a kernel path.)

A timer interrupt therefore never arrives in the middle of a syscall, an
IPC operation, a capability mutation or any other kernel code. If it
becomes pending while the kernel runs, it is taken right after the return
to user mode, or in the idle thread. **There is no kernel preemption, and
no context switch from arbitrary kernel execution.**

A **scheduling boundary** is the point where a kernel entry is complete
and the CPU picks what runs next, in `schedule()` or in the idle thread.
These are the only places threads switch.

**SC10. The preemption path:**

```
user code runs (IF=1)
  -> timer interrupt
  -> kernel entry: the T32 state, all registers saved to the TCB (T6)
  -> EOI
  -> expiry? (SC12)
       yes: sched_preempt(current)
       no:  ignore; the same thread continues
  -> scheduling boundary: dispatch (SC6)
  -> return to user mode through IRETQ (T18: interrupt entries never
     SYSRET)
```

FPU and vector state are saved at every entry and restored at dispatch,
as today.

**SC11. The quantum.** Every dispatch of a user thread stops the CPU's
LAPIC timer and arms it in one-shot mode for `QUANTUM` ticks, a nonzero
constant. Nothing else arms the timer, and the idle thread disarms it.
The timer keeps counting while the kernel runs, so kernel time spent on a
thread's behalf counts against its quantum. A thread that blocks or
yields loses the rest of its quantum.

**SC12. Expiry, exactly.** A timer interrupt is an expiry **if and only
if the LAPIC current-count register reads 0** when the interrupt is
handled. It is a stale interrupt otherwise, and is acknowledged and
ignored. This works because:
- one-shot mode counts down from the initial count to 0 and stays at 0,
  raising one interrupt on reaching 0
- every dispatch writes a nonzero initial count (SC11), with interrupts
  disabled, immediately before returning to user mode

So an interrupt latched for an earlier arming (pending while the kernel
ran, then delivered after the next dispatch re-armed the timer) finds a
nonzero count and is ignored. If the new arming has also run out by then,
the count is 0 and preempting is correct anyway. An interrupt that
arrives while the idle thread runs (timer disarmed, count 0) has no thread
to preempt. It wakes the idle thread, which dispatches if the queue is
not empty. No generation counter is needed: the hardware count is the
generation.

**SC13. Round-robin.** Each CPU's run queue is FIFO. A preempted,
yielding or newly woken thread goes to the tail (P12). There is one
priority level.

**SC14. Non-monopolisation bound (T36).** Let a thread X become RUNNABLE
on CPU c, entering the tail of its run queue at time t0. Let k be the
number of non-idle threads ahead of it at t0: those in the queue before
it, plus the thread RUNNING on c, if any. Then **X is dispatched by
t0 + k·(QUANTUM + W) + W ticks.** This holds because:
- **Threads behind X:** a thread made runnable after t0 (woken, resumed,
  preempted, yielding) enters behind X.
- **Threads ahead of X:** each holds the CPU at most once before X:
  - at most QUANTUM ticks of its quantum, measured by the timer,
    including the kernel entries made on its behalf
  - plus the rest of at most one kernel entry that was in progress when
    the quantum expired (at most W, SC16)
  - leaving the queue early (blocking, stopping, destruction) only
    shortens the wait
- **A stale interrupt** (SC12) neither preempts early nor extends a
  quantum, because every dispatch re-arms the timer.
- **The last W term** covers the scheduling boundary that dispatches X.

N (the number of non-idle threads RUNNABLE or RUNNING on c at t0) gives
k ≤ N - 1. The tests measure the bound in timer ticks (test 1).

## Continuations

**SC15. Kernel work models.** Every kernel operation is one of:
1. **Short:** done in one entry within a constant bound.
2. **Long, owned by a thread:** it does a bounded **chunk** per dispatch
   of that thread, carrying an explicit continuation in the thread's TCB
   (SC16).
3. **Long, owned by the kernel:** the teardown of a dying object, done
   in bounded chunks by the CPU's **reaper** (SC21), independent of any
   thread.
4. **User-continued:** revocation, which already returns `T_EAGAIN`; the
   caller's next call continues it. Its progress is the CDT itself.

No operation is restarted by re-executing its syscall instruction. User
registers are never rewound.

**SC16. A thread continuation** is a record in the TCB:

| Field | Content |
|---|---|
| `kind` | which operation, or NONE |
| captured inputs | every user input, copied once at the first chunk: register arguments and anything read from user memory, such as an offer or a reply message |
| target identity | the capability slot named, plus the identity of the object it named (physical address and a 64-bit object serial, SC20) |
| cursor | where the operation stands, when that is not already recorded in kernel objects |

Rules:
- **Capture once.** User memory and user registers are read only at the
  first chunk. Later chunks use only the captured copy. (This extends
  capture -> validate -> commit to capture -> bounded work -> commit
  checkpoint -> continuation.)
- **Re-validate each chunk.** Anything that can change between chunks is
  checked again from kernel state:
  - the target slot must still hold a capability to the same object
    (same address and serial) with the needed rights; otherwise the
    operation ends with the error a fresh lookup would give
  - liveness
  - slot emptiness
- **Commit only valid states.** Each chunk ends with every invariant
  holding. The final chunk commits the result. Earlier chunks commit
  only progress that stands on its own, such as a failed sender, which
  is that sender's own outcome (P53).
- **No result until the end.** The thread's user registers are untouched
  until the final chunk writes the result, exactly as a one-pass
  operation would.
- **One chunk per dispatch.** A thread with a continuation is RUNNABLE.
  When it is dispatched, the kernel runs its next chunk before returning
  to user mode, instead of entering user mode. After a chunk that does
  not finish, the thread goes to the tail of the run queue: a scheduling
  boundary.
- **Abandonment.** Stopping the thread (suspension, destruction, binding
  loss, P64) discards its continuation. The committed progress stays
  valid. On resume, the syscall returns `T_EINTR` (P30), as for any
  interrupted operation.
- **Audit:** a continuation exists only on a RUNNABLE or RUNNING thread,
  never on a stopped, blocked or idle one, and its captured inputs are
  zero beyond their lengths.

In this design exactly one operation needs a thread continuation: the
receive drain (SC23). The mechanism is general, so the next long thread
operation does not invent another one.

## Semantic counters

**SC17. Counts are maintained, not computed** (this changes ADR 0009
P27a's "computed, not cached"). Each endpoint and notification holds two
counters: the number of live capability records to it carrying READ, and
the number carrying WRITE, in every cspace, CNode and binding slot. Each
Frame's alias state (writable mappings and IPC-buffer bindings,
executable mappings: V6, P5) is held in a **frame metadata table**:
- kernel memory reserved at boot, one entry per physical page that can
  hold a Frame
- indexed by page number, because a Frame's memory is user data and
  cannot hold kernel state
- no allocation after boot

**SC18. Counter invariants.**
- **C1.** A count never exceeds the number of live capability records to
  the object.
- **C2.** A count never underflows; every decrement asserts the count is
  positive.
- **C3.** A count always equals the number of live records carrying that
  right. A zombie (SC21) carries no rights.
- **C4.** A count changes in the same step as the capability-table
  mutation that creates or deletes the record: `cap_derive_commit`,
  `cap_install`, binding, and the final removal of a record, whether by
  deletion or by a zombie starting. Rights never change in place.
- **C5.** A failed operation changes no count: counts change only in
  commit steps, which cannot fail.
- **C6.** A change in CDT parentage (SC19) changes no count.

Tests check every counter against the existing independent rescan of all
capability tables after every step: O(1) in the kernel, O(N)
verification in tests.

## Bounded kernel work

**SC19. The CDT no longer renumbers.** Today, deleting a capability with
descendants decrements the depth of every descendant (O(descendants)).
Revocation also walks the whole subtree to find its end before deleting
anything (O(subtree) on every call, even with budget 1). Both are
replaced; the tree means the same thing:
- **Depth gaps are allowed.** A record's parent is the nearest earlier
  record with a smaller depth. A child's depth must be greater than its
  parent's, not necessarily exactly one more. Deleting a record simply
  unlinks it: its descendants become its parent's, because that is now
  the nearest earlier record with a smaller depth. O(1).
- **Insertion keeps the rule.** A new child of P goes right after P,
  with the depth of P's current first child if P has children, otherwise
  P's depth + 1. Then no existing descendant of P can end up under the
  new child. O(1).
- **Revocation deletes the first child** of the target until it has
  none: one O(1) step per deleted record, with no end-of-subtree search.
  Finalisers that delete more records (O11) no longer need the re-scan.

This amends the CDT representation documented in `cap.h` and ADRs
0002/0003; the derivation relation itself is unchanged.

**SC20. Object serials.** Endpoints, notifications and CNodes carry a
64-bit serial taken from a global counter when they are created. A serial
is never reused, so a continuation can tell its target object from a new
one at the same address.

**SC21. Zombies and the reaper.** Some objects cannot be torn down in
constant time:
- an endpoint or notification with waiters to wake
- a CNode with occupied slots

Deleting the last capability to such an object, or the last capability
carrying the authority its waiters depend on, works like this:
- **Short part, in the deleter's entry:**
  - when the object dies, the record becomes a **zombie** in the same
    slot; for authority loss alone, it is deleted normally. Either way
    the counters drop (C3, C4)
  - the object is appended to its CPU's **reaper list**, and the delete
    returns
  - later operations see the authority as gone, so new waiters cannot
    join a doomed queue (P27: a count that reaches 0 never rises
    again, because derivation needs a source carrying the right)
- **Long part, owned by the reaper:** at every scheduling boundary on
  that CPU, and in its idle thread, the reaper does at most
  `REAP_CHUNK` units of teardown on the object at the head of its list:
  - wake doomed waiters with `T_EDEAD` (fault callers become FAULTED,
    P72)
  - delete the slots of a dying CNode
- **When the teardown is complete,** the zombie is removed from its slot
  and the CDT (an ordinary O(1) deletion), and the object is off the
  list.

What a zombie is:
- **It grants nothing:** every lookup treats it as absent for authority.
- **It occupies its slot:** nothing can be placed there until it is gone.
- **It is a CDT node:** it keeps the object's memory pinned as a child
  of its Untyped, so the Untyped cannot be reset or re-retyped over
  memory that is still being torn down (O10).
- `cap_delete` on a zombie slot returns `T_EBUSY` ("deletion in
  progress").
- Revocation that reaches a zombie runs reaper chunks for that object
  (helping), then returns `T_EAGAIN` until the zombie is gone. Revoking
  an Untyped therefore always finishes, and never reuses memory early.

Authority loss without object death (the last WRITE capability to a live
endpoint is deleted while receivers wait) puts the endpoint on the reaper
list with the doomed direction recorded. No zombie is needed, because the
object stays alive.

Nesting: a dying CNode whose slots hold more dying objects puts each of
them on the list as their last capabilities go. The reaper finishes
objects in list order. Every unit of work removes a waiter or a slot,
and none adds any, so the reaper always terminates.

**SC22. W, defined.** **W is the maximum kernel work between two
consecutive scheduling boundaries on a CPU**, measured in work units. A
unit is one iteration of any loop whose trip count is not a compile-time
constant, or one constant-time step. Between two boundaries a CPU does
at most:
- one kernel entry's fixed work (entry, dispatch, exit): a constant
- one short operation, or one chunk of a thread continuation
- one reaper chunk (`REAP_CHUNK`)

So W = `ENTRY_MAX + max(SHORT_MAX, DRAIN_CHUNK) + REAP_CHUNK`. Every loop
in the kernel whose trip count is not a compile-time constant charges a
per-CPU work counter. Test builds assert, at every scheduling boundary,
that the counter is at most W, then reset it. A mutant that removes a
chunk bound therefore fails mechanically.

### Bound per operation

| Operation | Problem today | Step 6 treatment | Bound |
|---|---|---|---|
| Normal IPC (send, recv, call, reply, signal, wait) | none: 32 words, 3 derivations | unchanged | constant |
| Liveness checks (P27) | count walks every copy: O(copies) | counters (SC17) | O(1) |
| W^X alias check (V6, P5) on map and bind | walks every copy of the frame: O(copies) | frame metadata table (SC17) | O(1) |
| Deleting a capability with descendants | depth renumbering: O(descendants) | depth gaps (SC19) | O(1) |
| `cap_revoke` | end-of-subtree search O(subtree) per call; budget 0 = unlimited | first-child deletion (SC19); budget capped at `REVOKE_CHUNK`, 0 meaning `REVOKE_CHUNK` | `REVOKE_CHUNK` per call, user-continued |
| Wake-all on destruction or authority loss (P26, P27; P37 #1) | O(waiters) in one entry | zombie + reaper (SC21) | `REAP_CHUNK` per boundary |
| CNode tree deletion (ADR 0006) | the whole tree in one entry | zombie + reaper (SC21) | `REAP_CHUNK` per boundary |
| Receive drain (P53; P37 #2) | O(failed senders) in one entry | thread continuation (SC23) | `DRAIN_CHUNK` per dispatch |
| Retype zeroing (O6) | up to 1 GiB in one entry | refused above `RETYPE_ZERO_MAX` bytes per call (SC24) | `RETYPE_ZERO_MAX` |
| Scheduler transitions | constant | unchanged | constant |
| AddressSpace / PageTable teardown | fixed table sizes (V15) | unchanged | constant |
| Audit, `runq_contains`, rescans | O(threads), O(slots) | test builds only | not part of W |

P37's two named exceptions become bounded chunks: P37 is amended to
point here.

**SC23. The receive drain as a continuation.** `recv` and `reply_recv`
capture their inputs at the first chunk:
- the endpoint slot and the object's address and serial
- the offer
- for `reply_recv`, the reply message, in a kernel copy

Each chunk then:
- re-validates the endpoint capability (SC16)
- fails at most `DRAIN_CHUNK` undeliverable senders, each exactly once
  (I-E2)

Once the head sender is deliverable, or the queue is empty:
- `recv` completes or blocks, exactly as in one pass
- `reply_recv` re-runs its reply-half checks (P63 phase 2) on the
  captured message, then its liveness check, and only then commits the
  reply and receives

Nothing of the server's changes before that final chunk. If the
re-validated reply half now fails, `reply_recv` returns that error with
its reply authority kept, as P63 already allows for drained senders.

**SC24. Retype is bounded by refusal, not chunked.** A retype whose
zeroed span (count × object size, Untyped children excluded) exceeds
`RETYPE_ZERO_MAX` returns `T_ERANGE` with nothing changed. The constant
is at least the largest single object: today a CNode of 2^12 slots,
256 KiB. Callers retype in several calls. Objects larger than
`RETYPE_ZERO_MAX`, such as future large pages, will need a chunked
design of their own, which this ADR does not provide.

## Queue depth (closes I-B1)

**SC25. Every queue holds each live thread at most once.** This follows
from SC1: each thread has one wait link and one run link, and its state
lets it use at most one of them. The audit checks it mechanically,
replacing today's fixed loop limit:
- each queue's length is at most the number of live threads
- no thread appears twice in any queue
- no thread is in two queues
- links are symmetric, and walking a queue never repeats a node
- the reaper list holds each dying object at most once

## Invariants introduced

| ID | Invariant | Rules |
|---|---|---|
| I-SC1 | Every thread is owned by exactly the place its state names, never two; `current` is never NULL; home CPU fixed | SC1-SC6 |
| I-SC2 | No context switch from arbitrary kernel execution: interrupts enabled only in user mode and across the idle `hlt` | SC9 |
| I-SC3 | A thread made runnable is dispatched within k·(QUANTUM+W)+W ticks (T36) | SC11-SC14 |
| I-SC4 | At most W work units between two scheduling boundaries; long operations commit only valid states and never re-read user input | SC15-SC24 |
| I-SC5 | Semantic counters equal the live rights-bearing records at every step | SC17, SC18 |
| I-SC6 | A zombie grants nothing, pins its object's memory, and is removed only when the object's teardown is complete | SC21 |
| I-B1 (closed) | IPC work, message size and queue depth are bounded | P37, SC22, SC25 |

## Amendments to earlier ADRs (on acceptance)

- **ADR 0009 P27a:** counts are maintained (SC17), and checked against
  the rescan. **P37:** the exceptions are bounded chunks (SC21, SC23).
- **ADR 0006:** the CNode-drain known limitation is retired (SC21).
  Zombies (O10) are introduced as that ADR anticipated.
- **ADR 0002/0003 and `cap.h`:** the CDT depth rule becomes "greater
  than the parent's" (SC19). Revocation and deletion are O(1) per
  record.
- **ADR 0008 T36:** satisfied by SC14 once the tests pass.
- **ADR 0005:** unchanged. This ADR realises its per-CPU idle thread and
  run queue.

## Tests required

1. **T36:** two and five threads that spin forever without a syscall.
   All make progress, in round-robin order, and every wait between
   dispatches is within the SC14 bound measured in timer ticks.
2. **Preemption state:** a preempted thread is RUNNABLE at the tail with
   every register intact (the preserve-style checker runs under
   preemption); it returns through IRETQ; FPU and vector state survive.
3. **The timer while the kernel runs:** the interrupt is taken after the
   return to user mode. A counter of interrupts with a kernel-mode CS
   stays at zero outside the idle thread.
4. **Stale interrupts (SC12):** a timer interrupt made pending before a
   dispatch does not preempt the new thread; one arriving after its
   quantum ends does.
5. **The idle thread:** with nothing runnable, `current` is the idle
   thread and the CPU halts. It wakes on an interrupt and dispatches. It
   cannot be named, resumed or suspended.
6. **IPC under preemption:** the ADR 0009 suites rerun with a short
   quantum; the audit holds after every step; a queued or blocked thread
   is never preempted.
7. **Continuations (SC16, SC23):**
   - `recv` and `reply_recv` with more than `DRAIN_CHUNK` undeliverable
     senders: results identical to one pass; each sender failed once
   - the offer rewritten in user memory between chunks: no effect
   - the endpoint slot re-filled with a different endpoint between
     chunks: the error a fresh lookup gives
   - the thread suspended between chunks: consistent state, `T_EINTR`
     on resume
8. **Zombies and the reaper (SC21):**
   - endpoint deletion with more than `REAP_CHUNK` waiters
   - authority loss with many receivers
   - a deep CNode tree
   - revoking the Untyped while zombies remain
   - in each: no memory reused early, every waiter woken exactly once,
     the zombie grants nothing, and `cap_delete` on it gives `T_EBUSY`
9. **CDT without renumbering (SC19):**
   - deleting a capability with a long chain of descendants is O(1)
   - parentage is unchanged in meaning
   - insertion next to gapped descendants
   - revocation by first-child deletion
   - all existing CDT tests keep passing
10. **Counters (SC17, SC18):** checked against the rescan after every
    step of every test, including transfer, binding, zombies and failed
    operations.
11. **W (SC22):** the per-boundary work counter stays within W across
    the whole suite and in purpose-built worst cases; stack high-water
    within budget (T33).
12. **Queue bounds (SC25):** queues of 0, 1 and many threads.

### Planned mutants

- Queues:
  - duplicate insertion into a run queue or a wait queue
  - a thread in two queues
  - a stale link left after removal
  - a queue cycle
  - queue length exceeding the number of live threads
  - removing the wrong thread
- Transitions:
  - preempting a queued or blocked thread
  - waking without enqueueing
  - enqueueing without a state change
  - dispatch not setting `current`, or blocking leaving it set
  - the idle thread enqueued, or made addressable
- Timer:
  - a stale interrupt treated as expiry
  - the quantum not re-armed on dispatch
  - preemption returning through SYSRET
  - interrupts enabled in kernel code outside the idle `hlt`
- Continuations:
  - re-reading user memory or registers after the first chunk
  - a missing re-validation of the target
  - a result written before the final chunk
  - a continuation surviving a stop
  - a sender failed twice
- Zombies and the reaper:
  - a zombie granting authority
  - zombie memory reused before teardown completes
  - a doomed queue accepting a new waiter
  - a reaper chunk exceeding `REAP_CHUNK`
- Counters:
  - not updated on derive, delete, binding or zombie creation
  - updated on a failed operation
  - changed by a parentage change
- CDT:
  - a child inserted with a depth that captures existing descendants
  - deletion still renumbering
- Work bounds:
  - a revoke budget of 0 left unlimited
  - a retype above `RETYPE_ZERO_MAX` accepted
  - any chunk constant removed (caught by the W counter)

## Implementation order

Each stage keeps every existing test passing before the next begins.
1. **Counters and the frame metadata table** (SC17, SC18), checked
   against the rescan. Then remove every O(copies) walk.
2. **The CDT without renumbering, and first-child revocation** (SC19).
   Cap the revoke budget and bound retype (SC24).
3. **Ownership, transitions and the idle thread** (SC1-SC8, SC25): the
   run queue moves into per-CPU data, the home CPU is added, the audit
   is extended. No timer yet, so behaviour is unchanged.
4. **Zombies and the reaper** (SC20, SC21).
5. **Thread continuations and the chunked receive drain** (SC16, SC23).
6. **Timer preemption and the quantum** (SC9-SC14), with the T36 tests.
   Then the W counter (SC22) over the whole suite.
7. **Mutation testing, review, documentation,** and the amendments
   above. T36 is then satisfied.
