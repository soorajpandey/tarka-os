# ADR 0010: Scheduler, timer preemption and bounded kernel work

Status: accepted (revision 3f; step 6), revision 3g proposed. Stages 1-5 implemented. Revision 3c was `2c517b1`.
Revisions 3d, 3e and 3f (sections "Revision 3d", "Revision 3e" and
"Revision 3f") amend it with the stage 3, 4 and 5 findings; revision 3g (proposed) with the stage 6 audit. Rule numbers SC1..SC32 are
referenced from code, tests and other ADRs.

History:
- **Revision 3g** (this one, proposed): the stage 6 audit of SC9-SC14
  and T36 against the code and against themselves. Gaps H1-H7: timer
  ownership, the arming point, quantum units, SC14's mixed units, T36's
  unchecked conditions, the idle reaping window, and tests that assume
  no preemption. The plan is H8-H12.
- **Revision 3f** (`5b8a70d`): the stage 5 audit of SC1-SC8 and SC29
  against the code: gaps G1-G5, the stage 5 plan (G6-G10), and the
  pairing rule G11. Proposed in `3b0c7fc`; accepted after review, with
  these added from it:
  - G3's lifetime ordering for a thread that destroys itself
  - G4's line between the pre-HLT test escape and the real halt
  - G5's O(1) limited to the scheduler's own state change
  - G8's audit derived independently of the scheduler's bookkeeping
  - G9's refusals that change nothing
  - G11 (new)
- **Revision 3e** (`16091f1`): four gaps found while planning stage 4
  (E1-E4), the stage 4 plan (E5-E10), and E0 as stage 4's master
  invariant. Proposed in `fef2565`; accepted after review, with these
  added from it:
  - E1's eight properties stated outright
  - E2's invariant worded exactly, its snapshot taken from the
    endpoint's own queue and carried as a cursor
  - E3's measure proved lexicographic: a chunk with no progress needs
    reaper progress before the next one
  - E4's impossible states listed
- **Revision 3d** (`b748a52`): three gaps found while planning stage 3,
  with their fixes (D1-D3), the identity/lifetime separation (D0), and
  the stage 3 plan (D4-D9). Proposed in `8be48cf`; accepted after
  review with two conditions, both written in: D1's invariant with
  "detach is not destruction", and a well-founded termination proof
  for D2.
- **Revision 1** (`5502c54`): first draft.
- **Revision 2** (`1fd873c`): idle thread, continuations, counters, W.
- **Revision 3c** (`2c517b1`) specifies the freeze lifecycle (SC22b): the
  invariant F1 on every CDT mutation; ownership; a second revocation;
  cancellation and `T_EINTR`; reaper-owned freezes; enclosing
  revocations; and the carving-chain proof. The freeze design and the
  removal of the revoke budget were approved in the review of 3b.
- **Revision 3b** (`8fd04a7`) was the pre-acceptance audit (section
  "Pre-acceptance audit"). It:
  - states the dying-object transition as a transaction (SC23)
  - completes the W table (SC26)
  - adds progress measures for every long operation (SC30)
  - adds implementation gates for the checks only code can pass (SC31)
  - **The audit found one real liveness hole:** a chunked revocation can
    be outrun forever by holders deriving new records into the subtree.
    Revocation becomes a thread continuation with a derivation freeze
    (SC22, SC22a). This changes revision 3's choice of a user-continued
    API.
- **Revision 3a** (`8e65a8d`) resolved the third review:
  - the CDT proof is a stand-alone induction (SC20)
  - the contiguity claim is removed: "last capability" is decided by a
    live-record count (SC19a)
  - the consequences of 128-byte records are accounted for, with size
    assertions (SC20a)
  - the dying-object ordering (R0) and non-interference (R8) are stated
  - `T_EAGAIN` from retype holds no hidden state
  - continuations hold no kernel pointers (SC16)
  - a budget-1 revoke test over a huge subtree is required (SC22)
- **Revision 3** (`e5e4e3e`) resolved the second review:
  - **The CDT representation (SC19, SC20).** Revision 2 allowed depth
    gaps. A proof attempt found a reachable counterexample (SC19), so
    revision 3 replaces depths with balanced brackets, which have a short
    proof.
  - **Revocation is O(budget)**, with no preliminary scan (SC22).
  - **Frame alias metadata** has a fixed capacity set at boot and cannot
    run out (SC18).
  - **W** is specified from named constants; the work counter only
    measures it (SC26, SC27).
  - **Dying objects** replace zombie capability records, with their own
    invariants R1-R7 (SC23, SC24).
  - **Object serials** have a defined lifetime, and exhaustion is an
    error (SC21).
  - **The continuation rule** is stated generally (SC15).

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
- `current` is per-CPU and NULL when nothing runs
- the kernel always runs with interrupts disabled (T10)
- an interrupt from user mode is acknowledged and the thread continues
- when nothing is runnable, the scheduler returns to a kernel
  continuation (the self-test driver)
- the LAPIC timer works in one-shot mode, armed by one test only
- ADR 0005 decided, for every CPU: a kernel stack, an **idle thread** and
  a run queue; and SMP phase 1 behind a big kernel lock

Unbounded kernel work found by reading the code:
- **in IPC:** wake-all and the receive drain (P37)
- **elsewhere:**
  - `cap_revoke` with budget 0
  - CNode tree deletion
  - retype zeroing up to 1 GiB
- **in the capability layer:**
  - deleting a capability with descendants renumbers every descendant
  - every revoke call first walks the whole subtree to find its end
  - the W^X alias check and count computation walk every copy of an
    object

## Scope

In scope:
- the scheduling state machine and its audit
- per-CPU run queues and idle threads, with only CPU 0 started
- round-robin with a fixed quantum, and timer preemption
- the non-preemptible kernel
- a bound W on kernel work between scheduling boundaries, covering
  every unbounded path above
- maintained counters, a bounded CDT, dying objects and the reaper
- the I-B1 queue-depth bound

Out of scope:
- starting other CPUs
- migration and load balancing
- priorities, budgets and scheduling contexts (MCS), deadlines
- IPC timeouts
- calibrating the quantum to wall-clock time
- large pages

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
| IDLE (idle threads only) | the `current` pointer of its CPU, when no other thread runs there |

No thread is owned by two places:
- no thread is on a run queue and in a wait queue or reply link
- no thread is `current` on a CPU and queued anywhere

**SC2. `current` is never NULL** once a CPU schedules. It is a RUNNING
user thread or that CPU's idle thread.

**SC3. Step 6 does not migrate threads.** Every thread has a home CPU,
recorded in its TCB when it is created (CPU 0 in this step) and never
changed. Invariant: **`t->home_cpu` is the CPU whose run queue or
`current` owns `t`** whenever it is RUNNABLE or RUNNING. Migration and
load balancing need an ADR of their own.

**SC4. No global `current`.** Every use of "the current thread" names a
CPU. The audit checks SC1-SC3 for every CPU.

## The idle thread

**SC5. Each CPU has one idle thread** (ADR 0005): a kernel-owned,
statically allocated TCB.
- No capability can name it, so no syscall, IPC operation or thread can
  affect it.
- Its state is IDLE, which no other thread can have. It is never
  RUNNABLE, queued, blocked, suspended or destroyed, and holds no reply
  authority.
- It runs in ring 0 on the CPU's kernel stack, as the CPU's `current`,
  and only when the run queue is empty. It gives up the CPU as soon as
  the queue is not.
- The quantum timer is disarmed while it runs.
- Its loop:

```
loop:
    if the run queue is not empty: dispatch (SC6)
    else if this CPU's reaper has work (SC24):
        one reaper chunk
        sti; nop; cli          an interrupt window between chunks
    else:
        sti; hlt; cli          sti;hlt is atomic: no wake-up is lost
        handle the interrupt that woke it
```

Test builds may leave the idle thread for the self-test driver once the
system is idle, but only through the idle thread, so the halt-and-wake
path is always exercised.

## Transitions

**SC6. One primitive per transition** (amended by revision 3f, G1 and
G11). Thread state, run-queue membership and each CPU's `current`
change only through these functions in `core/sched.c`. Each checks its
from-state and the links the edge needs, returns `T_EINVAL` with
nothing changed otherwise, and commits the state together with the
scheduler-owned membership. Kernel callers assert success.

| From | To | Primitive | Cause |
|---|---|---|---|
| INACTIVE, FAULTED | RUNNABLE | `sched_start(t)` | resume (SC8) |
| BLOCKED_* | RUNNABLE | `sched_wake(t)` | IPC wake (after IPC has unlinked it) |
| RUNNABLE | RUNNING | `sched_dispatch()` | the scheduling boundary; the idle thread if the queue is empty |
| RUNNING | RUNNABLE | `sched_yield(t)` | `yield`; a continuation chunk not finished (E4); preemption at stage 6 |
| RUNNING | BLOCKED_SEND, BLOCKED_RECV, BLOCKED_NOTIF | `sched_block(t, kind)` | IPC wait (IPC links the queue first, P9) |
| RUNNING, BLOCKED_SEND | BLOCKED_REPLY | `sched_block_reply(caller)` | a call rendezvous (P58) |
| RUNNING, BLOCKED_SEND, BLOCKED_REPLY | FAULTED | `sched_fault(t)` | no handler, a failed fault call, a fault reply other than label 0, unsafe saved state |
| any but IDLE | INACTIVE | `sched_stop(t, destroying)` | suspend, `exit`, destruction, binding loss: abandonment, IPC detach, the run queue, the state |
| (none) | IDLE | `sched_boot()` | each CPU's idle thread, at boot only |

Any pair not in the table is impossible. `sched_release_current(t)` hands
a stopped `current` to the idle thread (G3). The build gate
`scripts/check-sched.sh` fails if anything outside `sched.c` writes the
state, the run-queue links, `home_cpu` or `current`.

**SC7. Atomicity.** The kernel is not preemptible (SC9). Every transition
completes within one kernel entry with interrupts disabled. The audit
checks SC1-SC4 at scheduling boundaries.

**SC8. Only stopped threads can be started.**

```
INACTIVE  --resume--> RUNNABLE
FAULTED   --resume--> RUNNABLE
BLOCKED_* --resume--> refused (T_EBUSY)
RUNNING   --resume--> refused (T_EBUSY)
RUNNABLE  --resume--> refused (T_EBUSY)
IDLE      -- not addressable
```

Preemption applies only to the RUNNING thread of the CPU taking the
interrupt.

## Preemption

**SC9. The kernel is not preemptible.** Kernel code runs with interrupts
disabled (T10). Interrupts are enabled only:
1. in user mode
2. in the idle thread: across `hlt`, and in the one-instruction window
   between reaper chunks (SC5)

(Test builds also contain one deliberate `sti; nop; cli` window inside a
self-test. It is test code, not a kernel path.)

A timer interrupt therefore never arrives inside a syscall, an IPC
operation, a capability mutation or any other kernel code. If it becomes
pending while the kernel runs, it is taken right after the return to user
mode, or in the idle thread. There is no kernel preemption and no context
switch from arbitrary kernel execution.

A **scheduling boundary** is where a kernel entry is complete and the CPU
picks what runs next: in `schedule()`, or in the idle thread's loop.
These are the only places threads switch.

**SC10. The preemption path:**

```
user code (IF=1) -> timer interrupt
  -> kernel entry: the T32 state, all registers saved to the TCB (T6)
  -> EOI
  -> expiry? (SC12)  yes: sched_preempt(current)   no: ignore
  -> scheduling boundary: one reaper chunk (SC24), then dispatch (SC6)
  -> return to user mode through IRETQ (T18)
```

FPU and vector state are saved at every entry and restored at dispatch.

**SC11. The quantum.**
- **Arming:** every dispatch of a user thread stops the CPU's LAPIC
  timer and arms it in one-shot mode for `QUANTUM` ticks, a nonzero
  constant. Nothing else arms it, and the idle thread disarms it.
- **Kernel time counts:** the timer keeps counting while the kernel
  runs, so kernel work on a thread's behalf counts against its quantum.
- **Blocking or yielding** gives up the rest of the quantum.

**SC12. Expiry, exactly.** A timer interrupt is an expiry **if and only
if the LAPIC current-count register reads 0 when the interrupt is
handled**. Otherwise it is stale: acknowledged and ignored. This works
because:
- in one-shot mode the count runs down to 0, stays at 0, and raises one
  interrupt when it gets there
- every dispatch writes a nonzero initial count, with interrupts
  disabled, immediately before returning to user mode

So:
- an interrupt latched for an earlier arming and delivered after a
  re-arm finds a nonzero count and is ignored
- if the new arming has also run out by then, preempting is correct
- in the idle thread the timer is disarmed and there is nothing to
  preempt

The hardware count serves as the generation; no software generation
number is kept.

**SC13. Round-robin.** Each CPU's run queue is FIFO. A preempted,
yielding or newly woken thread goes to the tail (P12). One priority
level.

**SC14. Non-monopolisation bound (T36).** Let thread X enter the tail of
CPU c's run queue at time t0. Let k be the number of non-idle threads
ahead of it at t0: those queued before it, plus the thread RUNNING on c,
if any. Then **X is dispatched by t0 + k·(QUANTUM + W) + W ticks**,
where W is the bound of SC26:
- **Threads behind X:** a thread made runnable after t0 (woken, resumed,
  preempted, yielding, or continuing a chunked operation, SC16) enters
  behind X.
- **Threads ahead of X:** each holds the CPU at most once before X:
  - at most QUANTUM ticks, measured by the timer and including the
    kernel entries made on its behalf
  - plus the rest of at most one kernel entry in progress when the
    quantum ran out (at most W)
  - leaving the queue early (blocking, stopping, destruction) only
    shortens the wait
- **Reaper chunks** are part of W at each boundary (SC24).
- **A stale interrupt** neither preempts early nor extends a quantum.
- **The final W** covers the boundary that dispatches X.

If N is the number of non-idle threads RUNNABLE or RUNNING on c at t0,
then k ≤ N - 1.

## Long operations

**SC15. When a continuation is required.** An operation needs an
explicit continuation whenever it cannot complete within its permitted
per-boundary work (SC26) while keeping its externally atomic meaning.
Each long operation states which of the four mechanisms it uses:

| Mechanism | When it fits | Step 6 uses it for |
|---|---|---|
| **Thread continuation** (SC16) | the remaining work is part of the caller's result | the receive drain (SC25), revocation (SC22) |
| **User-continued API** | the operation has a natural partial result the caller can repeat, and stopping half-way leaves nobody holding authority they should have lost | none in step 6 (revocation fails the second condition, SC22) |
| **Kernel-owned teardown** (reaper, SC23-SC24) | the remaining work is not part of anyone's result | object death, and wake-all on authority loss |
| **Refusal above an architectural limit** | splitting the request is the caller's business | retype zeroing (SC28) |

No operation is restarted by re-executing its syscall instruction. User
registers are never rewound.

**SC16. A thread continuation** is a record in the TCB:

| Field | Content |
|---|---|
| `kind` | the operation, or NONE |
| captured inputs | every user input, copied once at the first chunk: register arguments and anything read from user memory |
| target identity | the slot named, plus the identity of the object it named: physical address and serial (SC21) |
| cursor | where the operation stands, as a stable logical position (for example "the head of the send queue"), when kernel objects do not already record it |

**A continuation holds no pointer to any kernel object.** It holds
values: captured inputs, a slot number, an (address, serial) pair and a
logical cursor. Every chunk reaches its target only by looking the
captured slot up again in the thread's current cspace (itself taken
from the thread's binding) and comparing address and serial. So an
object deleted between chunks is never dereferenced through the
continuation (no use-after-free), and an object created later at the
same address never matches (no ABA). Nothing needs pinning: a
continuation keeps no object alive and no object waits for a
continuation.

Rules:
- **Capture once.** User memory and user registers are read only at the
  first chunk: capture -> bounded work -> commit checkpoint ->
  continuation.
- **Re-validate each chunk.** The target slot must still hold a
  capability to the same object (address and serial) with the needed
  rights; otherwise the operation ends with the error a fresh lookup
  would give. Liveness and slot emptiness are checked again from kernel
  state.
- **Commit only valid states.** Each chunk leaves every invariant
  holding. Only the final chunk commits the result. Earlier chunks commit
  only outcomes that stand on their own, such as a failed sender (P53).
- **No result until the end.** User registers are untouched until the
  final chunk writes the result.
- **One chunk per dispatch.** A thread with a continuation is RUNNABLE.
  When dispatched, the kernel runs its next chunk instead of entering
  user mode. After a chunk that does not finish, the thread goes to the
  tail: a scheduling boundary.
- **Abandonment.** Stopping the thread discards the continuation. The
  committed progress stays valid. On resume the syscall returns
  `T_EINTR` (P30).
- **Audit:** a continuation exists only on a RUNNABLE or RUNNING thread.

## Semantic counters

**SC17. Counts are maintained, not computed** (this amends P27a). Each
endpoint and notification holds two counters: the number of live
capability records to it carrying READ, and the number carrying WRITE,
in every cspace, CNode and binding slot.

**SC18. Frame alias metadata, with fixed capacity.**
- **What an entry is.** Every Frame's W^X alias state (V6, P5) is held
  in a **frame metadata table**: one entry per **physical 4 KiB page**
  that can back a Frame, indexed by page number. A Frame's memory is user
  data, so it cannot hold kernel state.
- **What an entry records:** two counters for the Frame whose base is
  that page:
  - writable aliases: writable mappings plus IPC-buffer bindings
  - executable aliases: executable mappings

  Frames never overlap (O5), so one entry per base page cannot conflict.
  A future large Frame would use the entry at its base page; that
  belongs to the large-pages design.
- **Capacity.** The table is sized at boot to cover every page the
  physical allocator manages, which includes every range handed out as
  Untyped. *Corrected at stage 2:* an entry is 24 bytes, not 8, because
  it also holds the live-record count (SC19a), the freeze flag and the
  carving back-pointer (SC22a) of the Frame, AddressSpace or PageTable
  based there: about 0.6% of memory. It is reserved before any Untyped
  exists. **It cannot fill at
  run time**, because every page that can ever back a Frame has its
  entry from boot. There is no allocation, no lookup structure and no
  blocking. If the reserve cannot hold the table, boot fails, which is
  deterministic and visible.
- **Counter width.** A counter that would overflow makes the operation
  fail with `T_ERANGE`, with nothing changed. Each alias needs a 128-byte
  capability record (SC20), so 32-bit counters cannot overflow below
  512 GiB of RAM. The check makes this a deterministic error rather than
  an assumption.

**SC19a. The live-record count.** Every object that can have more than
one capability record keeps a count of all its live records, whatever
their rights. Deleting a record is the deletion of the **last**
capability to its object exactly when the count goes from 1 to 0. Where
the count lives:

| Object | Location |
|---|---|
| Endpoint, Notification | in the object (SC23) |
| CNode | the slot-property area of its slot 0 (SC20a) |
| Thread | in the TCB |
| AddressSpace, PageTable | the frame metadata table entry of its base page (SC18). *Corrected at stage 2:* this said "in the shadow page", which has no free space (all 512 slots are back-pointers) |
| Frame | the frame metadata table entry of its base page (SC18), alongside the alias counters |
| Untyped | its own record, which is unique (O2). The count is always 1; keeping it makes finality one rule for every type |
| Debug console, test token | no object, no finaliser; not counted |

C1-C6 apply to it as to the rights counters.

**SC19. Counter invariants.**
- **C1.** A count never exceeds the number of live records to the object.
- **C2.** A count never underflows; every decrement asserts the count is
  positive.
- **C3.** A count equals the number of live records carrying that right
  (or, for frames, the live aliases of that kind).
- **C4.** A count changes in the same step as the record or mapping
  change that causes it: derivation commit, install, binding, mapping,
  unmapping, deletion. Rights never change in place.
- **C5.** A failed operation changes no count: counts change only in
  commit steps, which cannot fail.
- **C6.** A change in CDT parentage changes no count.

Tests check every counter against the independent rescan of all
capability tables after every step.

## The capability derivation tree

**SC20. The CDT becomes a balanced-bracket list.**
- **Why revision 2's depth gaps are wrong.** Revision 2 kept depths but
  stopped renumbering on deletion, with "parent = nearest earlier record
  with smaller depth". That is not invariant under deletion. This
  sequence is reachable with ordinary derive and delete:

  ```
  derive x<-p, y<-x, z<-y        p0 x1 y2 z3
  derive m<-p                    p0 m1 x1 y2 z3      (new child goes first)
  derive w<-m                    p0 m1 w2 x1 y2 z3
  delete m                       p0 w2 x1 y2 z3      correct
  delete y                       p0 w2 x1 z3         z under x: correct
  delete x                       p0 w2 z3            z under w: WRONG (p)
  ```

  Depth alone cannot represent the tree once gaps exist. This exact
  sequence is a required regression test.
- **The representation.** Each capability record carries two list
  elements: an **open** element and a **close** element. The CDT is one
  doubly linked list of these elements (across all cspaces, as today),
  forming a **well-nested** bracket sequence. The tree is defined by
  nesting alone; there is no depth field, so the tree has a single
  source of truth.
- **Definitions.**
  - x's **descendants** are the records whose brackets lie strictly
    between open(x) and close(x).
  - x's **parent** is the record whose bracket pair most tightly
    encloses x's.
  - A record with no enclosing pair is a root.

Operations, all O(1):

| Operation | List change |
|---|---|
| derive c from p | insert open(c), close(c) immediately after open(p) |
| delete x | unlink open(x) and close(x) |
| p has children | the element after open(p) is not close(p) |
| p's first child | the record whose open element follows open(p), if any |

"Is x the last capability to its object?" is **not** a CDT question. It is
answered by the object's live-record count (SC19a). The CDT represents
derivation only.

**Proof** (independent of any model check). The invariant is **INV: B is
a well-nested bracket sequence in which each record contributes exactly
one open and one matching close element, and the parent relation read
from B (tightest enclosing pair) equals the abstract forest F.**

1. **Base.** A record created by retype or installed at boot is inserted
   as an adjacent pair `()`: at boot as a root, by retype immediately
   after open(Untyped). An adjacent pair is well-nested, encloses
   nothing, and is enclosed by exactly the pairs that enclose its
   insertion point. That is the Untyped (and that Untyped's ancestors),
   or nothing at boot. This matches F, which gets the new record under
   the Untyped, or as a root.
2. **Derive.** Deriving c from p inserts a complete pair `()` immediately
   after open(p), which lies strictly inside p's interval.
3. **So nesting is preserved.** Inserting a complete adjacent pair
   anywhere keeps a sequence well-nested. Because it goes right after
   open(p):
   - no pair begins between open(p) and open(c), so p's pair is c's
     tightest enclosing pair
   - c encloses nothing
   - every other pair's contents gain c exactly when they contain p

   B now reads F + {c -> p}.
4. **Removal changes nothing else.** Removing a pair x unlinks two
   elements and moves nothing. The relative order of every remaining
   element is unchanged, and so is, for every pair of remaining records,
   whether one encloses the other.
5. **x's children go to x's parent.** For a remaining record y:
   - **If x was y's tightest encloser,** the next tightest encloser of y
     before the deletion is x's tightest encloser, its parent: any pair
     enclosing y and enclosing x's interval must enclose x. So after the
     deletion y's parent is x's former parent, or y is a root if x was.
   - **Otherwise** y's tightest encloser is unchanged, by step 4.
6. **So the forest stays exactly the CDT.** After deleting x, B reads F
   with x removed and x's children re-parented to x's parent. That is
   the deletion semantics of P27a's "re-parenting", with no renumbering.
   By induction over any sequence of steps 1, 2 and 5, INV always holds.

**Corollary (revocation).** Revoking p deletes, at each step, the record
whose open element follows open(p): a descendant of p, by INV. By step 5
its children stay inside p's interval. Each step shrinks p's interval by
exactly one record, and p has no descendants exactly when the element
after open(p) is close(p). Every intermediate state satisfies INV.

**No contiguity is assumed.** Records of one object need not be
adjacent in B, and nothing in the kernel may rely on that. Under today's
rules they happen to be, because derivation always copies the source's
object and the only records with children of other objects are
Untypeds, which cannot be copied (O2). That is a coincidence of the
current rules, not an invariant. The existing uses of it
(`is_final()`, `cap_object_first()`) are removed in favour of SC19a.

**SC20a. Record layout.** The two list elements add 16 bytes, and the
dying-object bookkeeping of a CNode (SC23) needs slot-property space.
`struct cap` grows from 64 to **128 bytes**. Its size and alignment are
fixed by compile-time assertions (`sizeof(struct cap) == 128`, and every
slot array 128-byte aligned), so they cannot change by accident. The
fields are explicit:
- type, rights, slot properties
- object, and the type-specific word
- the open and close elements
- the mapping entry
- a slot-property area kept by `cap_slot_clear` (as `pending_link` is
  today)

Consequences, accounted for:

| What | Effect |
|---|---|
| CNode capacity | unchanged in slots: up to 2^12 slots per CNode |
| CNode memory | doubled: 2^n slots take 2^(n+7) bytes; the largest CNode is 512 KiB (this sets `RETYPE_ZERO_MAX`, SC28) |
| Boot-time capability memory | the root cspace (256 slots) grows from 16 KiB to 32 KiB of kernel BSS |
| TCB | four binding slots take 512 bytes; the TCB stays within page 0 of the thread object (about 1.2 KiB plus the continuation; asserted) |
| CDT storage | inside each record; nothing is allocated per derivation |
| Kernel stack | none: no kernel function keeps a `struct cap` on the stack (checked by `-fstack-usage` review, T33) |
| User ABI | slot numbers, capability identities and syscall arguments are unchanged |

**SC21. Object serials.**
- Every endpoint, notification and CNode carries a 64-bit serial when it
  is created, taken from a global counter that only increases.
- **A serial is unique for the lifetime of the system.** On exhaustion,
  when the counter would wrap, creating those objects fails with
  `T_ENOMEM`, deterministically, and never reuses a serial. (At one
  creation per nanosecond that takes over 500 years, but the rule does
  not depend on that.)
- Continuations compare (address, serial), so an object created later at
  the same address and in the same slot never matches (the ABA test,
  test 8).

**SC22. Revocation is a thread continuation.**
- **Why not user-continued.** Revision 3 made revocation user-continued
  (`T_EAGAIN`). That leaves the subtree open to derivation between calls:
  any holder of a GRANT-bearing record inside it can derive new children
  into it. Holders with more threads than `REVOKE_CHUNK` can add records
  faster than each call removes them, so the revocation never completes
  and the authority it was meant to remove stays live. A one-pass revoke
  cannot be interleaved like this; chunking introduced the hole.
- **The new contract.** `cap_revoke(slot)` runs to completion as a thread
  continuation (SC16), and the syscall returns `T_OK` only when the
  target has no descendants:
  - each chunk deletes at most `REVOKE_CHUNK` records, each being the
    target's current first child, found in O(1) (SC20)
  - there is no preliminary scan: a chunk is O(`REVOKE_CHUNK`), never
    O(subtree) + O(budget)
  - the syscall's budget argument is removed: the kernel, not the
    caller, decides the chunk size
  - abandonment (the revoker is stopped) ends the operation as SC16 says,
    with the records already deleted staying deleted (each deletion is
    committed)
- **Required test.** A subtree of thousands of records, in deep chains
  and wide fans, at least 4·W units in size (so a reintroduced pre-scan
  exceeds W in the first chunk). Every chunk deletes exactly
  `REVOKE_CHUNK` records (fewer only in the last), leaves the tree equal
  to a reference forest, and stays within W on the work counter. A second
  test runs many threads deriving into the subtree throughout; the
  revocation still completes.

**SC22a. The derivation freeze.** While a revocation is in progress, no
new record can enter the subtree it is emptying:
- **The flag.** Revoking a record r of object O sets O's **freeze** flag,
  kept with O's live-record count (SC19a).
- **The Untyped case.** Revoking an Untyped U sets U's freeze flag
  instead, in U's own record, which is unique (O2). U's descendants are
  the objects carved from U, and the Untypeds carved from those.
- **The check, in `cap_derive_check()`.** That is the one place every
  derivation passes: derive, transfer, binding and retype. A derivation
  or retype fails with `T_EBUSY`, nothing changed, if any of these is
  frozen:
  - the source object
  - for retype, the source Untyped
  - any Untyped on the source's carving chain

  The carving chain is the object's back-pointer to the Untyped it was
  carved from, then that Untyped's back-pointer, and so on. Each nested
  Untyped is strictly smaller and at least `UNTYPED_MIN_BITS` in size, so
  the chain has at most `UNTYPED_BOOT_MAX_BITS - UNTYPED_MIN_BITS` links:
  a constant. (SC23's back-pointers extend to every object type, at the
  SC19a locations.)
- **Lifetime.** The flag is cleared when the revocation completes or is
  abandoned. A revocation on an object or Untyped already frozen fails
  with `T_EBUSY`: one revocation at a time per object.
- **Precision, a deliberate trade-off.** Freezing O blocks derivation
  from every record of O, not only those inside r's subtree. A precise
  test ("is the source inside r's interval?") needs order labels on the
  bracket list, whose relabelling is O(1) only amortised. The
  over-approximation lasts only while a revocation is running, and that
  revocation makes progress at every dispatch of its thread, so it is
  bounded by the subtree's size. A holder of a record of O can use it to
  delay other holders' derivations from O, never to stop them for good,
  and never to affect any other object. **Flagged for review.**

**SC22b. The freeze lifecycle.**

**F1. The invariant.** If a record lies in a frozen derivation subtree,
no successful operation creates a new descendant of it until the freeze
is released. F1 is a property of CDT mutation, not of any one function.
Every way a capability record can come into existence is listed here,
and each enforces it:

| Mutation | Where | Enforcement |
|---|---|---|
| derive, IPC transfer, binding | `cap_derive_commit()`, inserting under the source record | `cap_derive_check()` refuses a frozen source (F2) |
| retype | the retype commit, inserting under the source Untyped | refuses a frozen source Untyped, or one on a frozen chain (F2) |
| boot install of root Untypeds | `cap_install()` | runs before any thread exists, so no freeze can exist; asserted |
| deletion, with re-parenting | `cdt_remove()` | **cannot violate F1:** it moves no element. Every remaining record stays inside exactly the intervals it was in (SC20, step 4), so no record enters a frozen subtree |

There is no capability move or other re-parenting operation. **Any future
operation that creates a capability record must insert through one of
these two commit functions** (gate SC31.5: a build check that
`cap_insert_child()` is called from nowhere else).

**F2. Why the check is exact enough, and bounded.** Claim: the
ancestors of any record of an object O are records of O, followed by the
Untypeds of O's **carving chain** (the Untyped O was carved from, the
Untyped that one was carved from, and so on up to a boot root), and
nothing else. Proof:
- O's first record is created by retype as a child of the Untyped it was
  carved from, U1.
- Every other record of O is derived from a record of O, because
  derivation copies the object, so its parent is a record of O.
- Deletion only re-parents a record to its former grandparent, which is
  again a record of O or U1.
- U1 itself cannot be deleted while it has descendants (O8).
- The same argument applied to U1 (an object carved from U2) gives U1's
  ancestors.

So a record of O lies in the subtree of a frozen root f exactly when f
is either a record of O, which O's freeze flag covers (over-approximated
to all of O, as agreed), or an Untyped on O's carving chain. Checking
"O frozen, or any Untyped on O's chain frozen" therefore catches every
derivation into any frozen subtree, and nothing outside O and its chain
is affected.

The check is bounded. Retype requires the new Untyped to be strictly
smaller than its source (`size_bits < ut_bits`), and no Untyped is
smaller than `UNTYPED_MIN_BITS`. So a chain has at most `CHAIN_MAX` =
`UNTYPED_BOOT_MAX_BITS - UNTYPED_MIN_BITS + 1` links, a compile-time
constant, and each link is one back-pointer read (SC19a locations; an
Untyped's own record holds its back-pointer).

**F3. Ownership.** Every freeze has exactly one owner, recorded with the
frozen flag in the object's metadata (SC19a), or in an Untyped's record:
- **A thread,** for a revocation (SC22). The frozen object records its
  owner as an internal reference. It is cleared before the thread can
  be freed, like a wait-queue link (O10). The owning thread's
  continuation names the frozen object only by value (address and
  serial, SC16).
- **The reaper,** for an Untyped torn down inside a dying CNode (SC24).

**F4. One revocation at a time.** A revocation whose root object is
frozen, or whose root lies under a frozen Untyped on its chain, fails at
once with `T_EBUSY`, with nothing changed: no second continuation, no
freeze reference count. In particular:
- **Same object:** a second `revoke` on any record of an object already
  being revoked gets `T_EBUSY`.
- **A descendant of an Untyped being revoked:** the revoke reaches the
  ancestor's freeze through the chain check and gets `T_EBUSY`.
- **An ancestor of a running revocation:** for example, revoking U0 while
  a thread revokes a record r of an object carved under U0. The outer
  revocation is allowed: U0 itself is not frozen. When it reaches r, it
  **cancels** the inner revocation (F5) and deletes r. The enclosing
  revocation wins, and never waits for the enclosed one.

**F5. The root of a revocation stays put, or the revocation is
cancelled.**
- `cap_delete` on the root record of a running revocation returns
  `T_EBUSY`. That root therefore remains for as long as the revocation
  runs, and a frozen object cannot die (its root is one of its records).
- The only other ways the root can go are:
  - an enclosing revocation reaching it (F4)
  - the reaper deleting it from a dying CNode, when the CNode holding
    the root itself dies

  Both **cancel** the revocation, in one step: release the freeze, clear
  the owner reference, and mark the owner's continuation cancelled.
  Then delete the root. When the owner next runs, its revocation ends
  with `T_EINTR`. Cancelling is O(1) and never waits.

**F6. Abandonment is a transaction.** Every path that stops or destroys
the owning thread runs, before the thread can become unreachable or be
freed, a single step. These are suspension, `exit`, destruction and
binding loss, through the same funnel as P64's `ipc_detach()`. (A thread
with a continuation never runs user code, so it cannot fault while it
holds one.) The step:
1. locates the frozen object from the continuation's (address, serial).
   This is safe because a frozen object cannot die (F5), and is checked
   against the recorded owner.
2. releases the freeze and clears the owner reference
3. discards the continuation
4. leaves `T_EINTR` as the syscall's result for a later resume (not when
   destroying)

No revocation progress is rolled back: the records already deleted stay
deleted, each deletion having been committed. **So no freeze outlives its
owner:** the audit checks that every frozen flag names a live owner (a
thread whose continuation is that revocation, or the reaper with the
object listed) and that every revocation continuation owns exactly one
freeze.

**F7. Reaper-owned freezes.**
- When the reaper meets an Untyped with descendants in a dying CNode, it
  freezes that Untyped (owner: the reaper) and deletes its descendants,
  first child first, one per unit. It then deletes the Untyped, which
  releases the freeze.
- The reaper cannot be stopped, so such a freeze has no abandonment
  path. It is released exactly when the Untyped's record is deleted.
- The Untyped is unreachable (its CNode is dying), so nobody else can
  revoke or retype from it. Holders of descendants still cannot derive
  into it, because of F2.
- Reaper teardown **only deletes,** never creates records. It never
  waits for a freeze or for another dying object: if it reaches the root
  of a thread-owned revocation, it cancels that revocation (F5).

## Dying objects and the reaper

**SC23. Dying objects.** Some objects cannot be torn down in constant
time:
- an endpoint or notification with waiters to wake
- a CNode with occupied slots

When the last capability to such an object is deleted (its live-record
count reaches 0, SC19a):
1. **In the deleter's entry, O(1):**
   - the record is deleted normally; its slot is empty at once and
     reusable
   - the counters drop
   - the object becomes **dying**: appended to the deleting CPU's reaper
     list
   - the **dying count** of the Untyped it was carved from goes up
2. **Owned by the reaper:** the object's teardown (SC24).

Authority loss without object death (the last WRITE capability to a live
endpoint is deleted while receivers wait) puts the live endpoint on the
reaper list with the doomed direction recorded. A count that has reached
0 never rises again, because derivation needs a source carrying the
right, so a doomed queue only shrinks.

How the memory stays safe:
- **Back-pointer to the Untyped.** Endpoints, notifications and CNodes
  record, at retype, a pointer to the capability record of the Untyped
  they were carved from. That record is stable for the object's whole
  life, including while it is dying:
  - an Untyped capability cannot be copied (O2)
  - an Untyped with children or dying objects cannot be deleted (O8,
    amended)
- **Amendment to O7 and O8:** an Untyped's memory becomes reusable, and
  the Untyped deletable, only when it has **no children and a dying
  count of 0**. A retype that needs to reset the Untyped's watermark
  while dying objects remain returns `T_EAGAIN` with nothing changed.
  The count falls as the reaper finishes, and only the Untyped's own
  holder can add dying objects to it.
- **Where the bookkeeping lives.** Endpoints and notifications grow to
  128 bytes for:
  - the counters, serial and Untyped back-pointer
  - the reaper link and the doomed direction

  A CNode keeps its reaper link, back-pointer, serial and drain cursor in
  the slot-property area of its slot 0 (SC20a), which survives clearing
  slots, as `pending_link` does today.

**SC24. The reaper.** Each CPU has one. It is kernel code, not an object
and not a thread:
- At every scheduling boundary on its CPU, and in the idle loop, it
  performs **at most `REAP_CHUNK` units** on the object at the head of
  its list. A unit is one of:
  - waking one doomed waiter (with `T_EDEAD`; a fault caller becomes
    FAULTED, P72)
  - deleting one slot of a dying CNode: an ordinary deletion, which may
    make another object dying, appended to the tail
  - for a slot holding an Untyped that still has descendants, deleting
    that Untyped's first child (the Untyped is frozen, SC22a, and
    deleted as a unit once it has no descendants). Today's
    `cspace_destroy` does `revoke(c, 0)` here, which is unbounded.

  Each unit costs at most `UNIT_MAX`: the cost of the most expensive
  single deletion, which is an AddressSpace or PageTable finaliser over
  its 512 entries (V15).
- When an object's teardown is complete, the reaper takes it off the
  list. If it was dying, it decrements its Untyped's dying count, and the
  object's memory may then be reused by the rules above.

**R0: the death of an object is one transaction.** Every dying object
has exactly one owning Untyped, the one named by its back-pointer. The
deletion of its last record is a single non-preemptible commit step, and
no kernel observer (another entry, an interrupt handler, the reaper, the
audit) can see any state in between:

| | Before the commit | After the commit |
|---|---|---|
| Reachability | the object is live; its live-record count is 1 | no capability refers to the object; count 0 |
| Dying mark | not dying | marked dying |
| Untyped | dying count = n | dying count = n + 1 |
| Reaper | not listed (or listed for a doomed direction only) | listed on exactly one reaper |
| Slot | holds the last record | empty and reusable |

So these states can never be observed:
- a slot free while its object is not yet counted as dying
- an object dying while its memory is not pinned by its Untyped's count

The other two transitions are transactions too:
- **Teardown completion**, in one reaper unit: the object leaves its
  reaper list and its Untyped's dying count drops, together. Only after
  that commit may the memory be reset (amended O7).
- **Authority loss**, in one step: the opposite-direction count reaches 0
  and the live object is listed with its doomed direction.

The audit checks, for every Untyped, that its dying count equals the
number of listed dying objects whose back-pointer names it.

**`T_EAGAIN` from retype is a plain retryable error.** It creates no
continuation, queue entry, reservation or other kernel state: nothing
changed, and the caller decides whether and when to retry. Retype never
blocks.

**Non-interference (R8).** Only operations authorised by an Untyped's
own capability can raise that Untyped's dying count:
- an object carved from U can exist only through a retype that used U's
  capability
- its last record can be deleted only by whoever holds that record

So a task that holds neither U's capability nor any capability to an
object carved from U cannot make a retype on U return `T_EAGAIN`, and
cannot lengthen the time it keeps doing so. Each dying object of U is
finished after at most the reaper work ahead of it when it was listed
(R7). Tasks that hold capabilities to objects carved from U, because U's
holder gave them, can delay U's reset only by the teardown of those
objects: work the holder chose to delegate.

Reaper invariants:
- **R1.** Every dying object, and every live object with a doomed queue,
  is owned by exactly one CPU's reaper: the CPU where it became so. It
  never moves to another reaper.
- **R2.** It appears on that reaper's list at most once: its reaper link
  is set exactly while it is listed.
- **R3.** It grants nothing. No capability refers to a dying object, and
  no capability carries the doomed direction of a doomed queue.
- **R4.** Its memory is not reused until its teardown is complete: the
  Untyped's dying count blocks reset and deletion (amended O7/O8).
- **R5.** Reaper work is at most `REAP_CHUNK` units per scheduling
  boundary. Thousands of dying objects take proportionally many
  boundaries, and each boundary returns to the scheduler, or with
  interrupts open in the idle loop.
- **R6.** Stopping or destroying the thread that caused the death has no
  effect on the teardown.
- **R9.** The reaper never waits for a freeze. It cancels a thread-owned
  revocation whose root it must delete (F5), and its own freezes are
  released when it deletes their Untyped (F7).
- **R7.** The reaper creates no dependencies:
  - It is not an object, holds no capability, and cannot be named,
    deleted or blocked.
  - Teardown never waits for another dying object: there are no zombie
    records in slots, so a dying CNode's slots never hold anything that
    must outlive it.
  - Every unit removes a waiter or a slot and adds none to the object
    being processed. Objects added later go behind it.

  So every listed object is finished after finitely many boundaries: at
  most the units ahead of it when it was listed, plus its own.

## The receive drain, and retype

**SC25. The receive drain is a thread continuation** (SC16). `recv` and
`reply_recv` capture their inputs at the first chunk:
- the endpoint slot, with the object's address and serial
- the offer
- for `reply_recv`, the reply message, in a kernel copy

Each chunk re-validates the endpoint capability and fails at most
`DRAIN_CHUNK` undeliverable senders, each exactly once (I-E2). Once the
head sender is deliverable, or the queue is empty:
- `recv` completes or blocks, exactly as in one pass
- `reply_recv` re-runs its reply-half checks (P63 phase 2) on the
  captured message and its liveness check, and only then commits the
  reply and receives

**SC28. Retype has an architectural limit.** A retype whose zeroed span
(count × object size; Untyped children excluded) exceeds
`RETYPE_ZERO_MAX` returns `T_ERANGE`, with nothing changed.
- `RETYPE_ZERO_MAX` is at least the largest single object, today a CNode
  of 2^12 slots (512 KiB with SC20a's 128-byte slots).
- **This is an architectural syscall limit chosen for the scheduler's
  work bound,** not a hardware limit and not a limit on what Tarka can
  represent. Callers split larger requests across calls.
- Raising it, or adding objects larger than it (large pages), requires
  revisiting W (SC26) in the same change.

## The work bound W

**SC26. Specification.** **Every scheduling boundary is reached after at
most W units of kernel work since the previous one,** on each CPU. A
unit is a constant amount of work independent of any user-controlled
quantity. Between two boundaries a CPU performs:
- one entry's fixed work: entry, exit and dispatch, `ENTRY_MAX`
- at most one of:
  - a short operation
  - one continuation chunk
  - one revocation call
- one reaper chunk

So **W = ENTRY_MAX + max(op bounds below) + REAP_CHUNK**, where every
bound is derived from named constants:

| Operation | Bound (units) |
|---|---|
| syscall entry and dispatch, interrupt entry and EOI, exit to user, timer expiry check | `ENTRY_MAX` (const) |
| yield, exit (`ipc_detach`) | const |
| identify | const |
| derive, transfer of one capability, binding (including the SC22a chain check) | `CHAIN_MAX` + const |
| delete one record (cleanup hook, counters, finaliser or hand-off to the reaper) | `UNIT_MAX` |
| revocation chunk | `REVOKE_CHUNK`·`UNIT_MAX` |
| retype | `RETYPE_ZERO_MAX`/64 (zeroing, cache lines) + (`RETYPE_ZERO_MAX`/`MIN_OBJECT`)·const (initialisation and installs) + `CHAIN_MAX` |
| map, unmap, protect, install, uninstall | const (four table levels; alias counters O(1)) |
| bind, unbind, resume, suspend, read/write registers, read fault | `TS_COUNT`·`UNIT_MAX` + `sizeof(struct user_regs)` copy |
| debug write | `DEBUG_WRITE_MAX` copy + output |
| send, call, reply | `MSG_MAX_WORDS` + `MSG_MAX_CAPS`·(derive) + const |
| receive drain chunk, `reply_recv` final chunk | `DRAIN_CHUNK`·(2·`MSG_MAX_CAPS`) + one send's bound |
| signal, wait, poll | const |
| fault delivery (P67-P69) | `FAULT_WORDS` + const |
| scheduler transitions | const |
| reaper chunk | `REAP_CHUNK`·`UNIT_MAX` |

Where `CHAIN_MAX` = `UNTYPED_BOOT_MAX_BITS - UNTYPED_MIN_BITS`, and
`UNIT_MAX` = 512 entries plus a constant (the largest finaliser). Every
syscall of ADR 0008 and 0009, every interrupt and exception entry, and
both kinds of kernel-owned work (reaper, timer) appear in this table.
Audit, rescans and `runq_contains` are test-build only and excluded.

P37's two named exceptions become the bounded rows of this table:
wake-all through the reaper, the drain through SC25. P37 is amended to
point here. **I-B1 is closed by SC26 and SC29.**

**SC27. Verification** (separate from the specification):
- **The work counter.** Test builds instrument every loop whose trip
  count is not a compile-time constant with a per-CPU work counter. At
  each scheduling boundary they assert the counter is at most W, then
  reset it. The counter measures; it does not define W. A wrong bound in
  the table is a specification bug, not something the counter can
  detect.
- **Tests of the worst cases:**
  - largest messages
  - full drains
  - full revocation chunks
  - maximum retype spans
  - full reaper chunks
- **Code review rule.** Every such loop cites the SC26 row that bounds
  it. A loop with no row is a defect.

## Progress

**SC30. Every long operation has a progress measure.** Each operation
names a non-negative quantity that every non-final step strictly
decreases, and that nothing outside the operation can increase. So no
step is repeated over the same state, and the operation finishes after
at most that many steps:

| Operation | Measure | Why it cannot grow |
|---|---|---|
| Revocation (SC22) | records in the target's subtree | the derivation freeze (SC22a): no record enters while it runs |
| Reaper (SC24) | total units of all listed objects: doomed waiters, plus occupied slots of dying CNodes, plus descendants of their frozen Untypeds | doomed queues cannot gain waiters (a count at 0 stays at 0); a dying CNode is unreachable, so nothing can be placed in it; its Untypeds are frozen; a unit that lists a new object adds that object's units behind, not to any earlier object |
| Receive drain (SC25) | for each queued sender, its position in the queue | each chunk removes senders only at the head, never re-examines a failed sender (I-E2), and new senders join behind |

**Receive-drain completion is not a scheduler liveness guarantee.** The
scheduler guarantees CPU fairness and bounded work per boundary. It
does not guarantee that the environment eventually supplies a
deliverable message. Revocation is different: its target set is fixed
by the freeze, so it is guaranteed to finish.

The receive drain, stated exactly: It ends as soon as a
deliverable sender reaches the head, or the queue is empty. A deliverable
sender queued at position k is reached within ⌈k/`DRAIN_CHUNK`⌉ chunks.
If every arriving sender is undeliverable (for example, holders of WRITE
capabilities who keep sending capabilities the receiver does not
accept), the receive keeps failing them on its own quanta and does not
complete. That is the same outcome as a one-pass receive that blocks
waiting for a deliverable message, except that it spends the receiver's
own time rather than none. It never takes more than one round-robin share
of the CPU, so no third party is affected (SC14).

## Implementation gates

**SC31. Checks that only code can pass.** Some properties of this ADR
are about the implementation, not the design. Each stage of the
implementation order is complete only when its gate passes:
1. **CDT correspondence (stage 1).** Every CDT function names, in a
   comment, the proof step of SC20 it implements, and does nothing else
   to the list. After every CDT operation in the test suite, the bracket
   list is compared with a reference forest maintained by the test,
   including the revision-2 counterexample and random sequences.
2. **No positional finality (stages 1-2).** "Last record" is decided
   only by the live-record count (SC19a).
   - A build check script (like `scripts/check-stac.sh`) fails if
     `is_final`, `cap_object_first`, or a same-object neighbour
     comparison reappears in kernel sources.
   - A mutant that decides finality from list position must be caught.
3. **Counters in the commit (stage 2).** Every counter change sits in the
   same commit function as its record or mapping change.
   - The rescan comparison runs after every audited step.
   - There is one mutant per mutation site (derive, install, binding,
     map, unmap, delete, hand-off to the reaper), each of which must be
     caught.
4. **The revocation pre-scan mutant (stage 1).** With the test subtree of
   SC22 (at least 4·W units), reintroducing an end-of-subtree scan before
   the first deletion must exceed W on the work counter.
5. **One insertion path (stage 2).** A build check fails if
   `cap_insert_child()` is called from anywhere except the derivation
   commit and the retype commit, the two places F1 is enforced (SC22b).
6. **What counts as a kill (every stage; adopted at stage 5
   acceptance).** A mutant is killed only by a behavioural failure of the
   test suite as it runs: a failed check, an audit that rejects, or a
   panic reached by running the tests.
   - These do not count: a mutant that fails to build or link, a harness
     error or timeout before the suite starts, or a failure only of an
     assertion inside the mutated code.
   - Each mutant is run twice, once as built and once with the mutated
     subsystem's own assertions disabled. It is caught only if it is
     caught in both runs.
   - The harness records why each mutant was killed, and treats a run
     that never printed `selftest: running` as not run.
   - An unexplained kill is investigated like a survivor.

## Pre-acceptance audit (revision 3b)

The review's checklist, and what this revision does about each item:

| # | Item | Result |
|---|---|---|
| 1 | CDT proof matches the list operations | Design: the operations table of SC20 is exactly the proof's steps. Code: gate SC31.1 |
| 2 | Last capability only by count; no positional logic | Design: SC19a, I-SC8, contiguity removed. Code: gate SC31.2 |
| 3 | Every counter change in the same commit as its table change | Design: C4, C5. Code: gate SC31.3 |
| 4 | Dying accounting never half-committed | **Changed:** R0 is now a transaction, with before/after states, plus teardown and authority-loss transactions |
| 5 | Reaper work bounded by `REAP_CHUNK` | **Changed:** a unit is defined, including the Untyped-in-dying-CNode case (today unbounded), and costs at most `UNIT_MAX` |
| 6a | Freeze lifecycle (review of 3b) | **Specified in 3c:** SC22b, F1-F7 and R9 |
| 6 | Every cursor has a progress measure and cannot repeat forever | **Failed for revocation, fixed:** SC22, SC22a, SC30. Reaper and drain measures stated; the drain's behaviour under endless undeliverable senders made explicit |
| 7 | Every operation's work represented in W | **Changed:** SC26 now lists every syscall, every entry kind and all kernel-owned work, with `CHAIN_MAX` and `UNIT_MAX` |
| 8 | The revoke pre-scan mutant is caught | Design: test subtree at least 4·W units. Code: gate SC31.4 |

## Queue depth

**SC29. Every queue holds each live thread at most once.** This follows
from SC1: one wait link and one run link per thread, and at most one in
use. The audit checks it mechanically, replacing today's fixed loop
limit:
- each queue's length is at most the number of live threads
- no thread appears twice in any queue, and none is in two queues
- links are symmetric, and no walk repeats a node
- the reaper list holds each object at most once (R2)

## Invariants introduced

| ID | Invariant | Rules |
|---|---|---|
| I-SC1 | Every thread is owned by exactly the place its state names; `current` is never NULL; the home CPU is fixed | SC1-SC6 |
| I-SC2 | No context switch from arbitrary kernel execution | SC9 |
| I-SC3 | A thread made runnable is dispatched within k·(QUANTUM+W)+W ticks (T36) | SC11-SC14 |
| I-SC4 | At most W work between scheduling boundaries; long operations never re-read user input and commit only valid states | SC15, SC16, SC22-SC28 |
| I-SC5 | Counters equal the live rights-bearing records and aliases at every step | SC17-SC19 |
| I-SC6 | The bracket list is well-nested and represents exactly the derivation forest | SC20 |
| I-SC7 | Dying objects grant nothing, are owned by exactly one reaper and exactly one Untyped, are counted by that Untyped before they become unreachable and until their teardown completes, and their memory is not reused before then | SC23, SC24 (R0-R8) |
| I-SC8 | "Last capability to an object" is decided by the live-record count, never by CDT position | SC19a, SC20 |
| I-B1 (closed) | IPC work, message size and queue depth are bounded | P37, SC26, SC29 |

## Amendments to earlier ADRs (applied on acceptance)

- **ADR 0009:**
  - P27a: counts are maintained and checked against the rescan
  - P37: the exceptions are bounded (SC26)
- **ADR 0006:**
  - O7, O8: the dying count (SC23)
  - the CNode-drain known limitation is retired
  - the "zombie capabilities" anticipated there are replaced by dying
    objects, which need no slot
- **ADR 0002/0003 and `cap.h`:**
  - the CDT is a balanced-bracket list (SC20)
  - `struct cap` grows to 128 bytes (SC20a)
  - deletion and revocation are O(1) per record
- **ADR 0008:**
  - T36 is satisfied by SC14 once its tests pass
  - the syscall table: `SYS_CAP_REVOKE` loses its budget argument and
    returns only when the revocation is complete (SC22)
- **ADR 0003:** "revocation is preemptible and restartable" is realised
  as a thread continuation with a derivation freeze (SC22, SC22a).
- **ADR 0005:** unchanged; this ADR realises its per-CPU idle thread and
  run queue.

## Tests required

1. **T36:** two and five threads spinning without syscalls. All make
   progress, round-robin, and every wait is within the SC14 bound
   measured in timer ticks.
2. **Preemption state:** a preempted thread is RUNNABLE at the tail with
   every register intact (the preserve-style checker under preemption);
   it returns through IRETQ; FPU and vector state survive.
3. **The timer while the kernel runs:** the interrupt is taken after the
   return to user mode. Interrupts with a kernel-mode CS: zero, outside
   the idle thread.
4. **Stale interrupts (SC12):** an interrupt pending before a dispatch
   does not preempt the new thread; one after its quantum ends does.
5. **The idle thread:** `current` is the idle thread when nothing is
   runnable; it halts, wakes on an interrupt and dispatches; it runs
   reaper chunks with interrupt windows; it cannot be named, resumed or
   suspended.
6. **IPC under preemption:** the ADR 0009 suites rerun with a short
   quantum; the audit holds after every step.
7. **The CDT (SC20):**
   - the tree A{B{C{D}, E}}: delete each of B, C, D, E in turn, and in
     several orders; after each deletion, the parent of every remaining
     record equals the expected forest
   - the revision-2 counterexample sequence: z must end under p
   - repeated deletion and re-derivation into the same slots
   - deleting a record whose descendants survive
   - derivations across several CNodes and cspaces
   - revocation by first-child deletion, checking the tree after every
     deletion
   - the live-record count and the "last record" decision after random
     derive and delete sequences, compared against a reference forest
     and reference counts kept by the test
   - all existing CDT, revocation and transfer-conservation tests
8. **Continuations (SC16, SC25):**
   - drains of more than `DRAIN_CHUNK` undeliverable senders: results
     identical to one pass; each sender failed once
   - the offer rewritten in user memory between chunks: no effect
   - **ABA:** the target endpoint deleted and a new one retyped at the
     same address into the same slot between chunks: the operation fails
     with the lookup error and never acts on the new object
   - the thread suspended between chunks: consistent state, `T_EINTR`
     on resume
9. **Dying objects and the reaper (SC23, SC24):**
   - an endpoint deleted with more than `REAP_CHUNK` waiters
   - authority loss with many receivers
   - a deep CNode tree
   - for each:
     - the slot is reusable at once
     - every waiter is woken exactly once
     - a retype that needs a reset gets `T_EAGAIN` until the dying
       count is 0, and never reuses the memory early
     - the Untyped cannot be deleted meanwhile
     - stopping the deleter changes nothing
10. **Counters (SC17-SC19):** checked against the rescan after every step
    of every test, including transfer, binding, mapping, deletion and
    failed operations; overflow refused.
11. **W (SC26, SC27):** the work counter stays within W in every test and
    in the worst cases above; stack high-water within budget (T33).
12. **Queues (SC29):** 0, 1 and many threads.
13. **Serials (SC21):** a forced near-exhaustion counter makes creation
    fail with `T_ENOMEM` and never reuses a serial.
14. **The derivation freeze (SC22a):** during a revocation, derive,
    transfer, binding and retype from the frozen object or from any
    object on a frozen carving chain return `T_EBUSY` with nothing
    changed; other objects are unaffected; the flag clears on completion
    and on abandonment; a second revocation on the same object gets
    `T_EBUSY`.
15. **The freeze lifecycle (SC22b):**
    - the revoking thread suspended, destroyed, exiting, or losing its
      cspace binding mid-revocation: the freeze is released, `T_EINTR`
      on resume, deleted records stay deleted, and the audit finds no
      ownerless freeze
    - `cap_delete` on the root of a running revocation gets `T_EBUSY`
    - an enclosing Untyped revocation cancels an inner one when it
      reaches its root, and the inner revocation ends with `T_EINTR`
    - a revocation on a descendant of a frozen Untyped gets `T_EBUSY`
    - a dying CNode holding a revocation's root cancels it
    - a dying CNode holding an Untyped with descendants: the reaper's
      freeze holds while it runs and is released when the Untyped goes

### Planned mutants

- Queues:
  - duplicate insertion
  - a thread in two queues
  - a stale link
  - a cycle
  - length above the live-thread count
  - removing the wrong thread
- Transitions:
  - preempting a queued or blocked thread
  - waking without enqueueing
  - enqueueing without a state change
  - dispatch not setting `current`, or blocking leaving it set
  - the idle thread enqueued or addressable
- Timer:
  - a stale interrupt treated as expiry
  - no re-arm on dispatch
  - preemption returning through SYSRET
  - interrupts enabled in kernel code outside the idle windows
- Continuations:
  - re-reading user input after the first chunk
  - a missing target re-validation, or the serial not compared (ABA)
  - an early result
  - a continuation surviving a stop
  - a sender failed twice
- CDT:
  - a child inserted after close(p) instead of after open(p)
  - deletion unlinking only one bracket
  - a revoke scan reintroduced
  - a wrong "last record" test
- Dying objects:
  - a dying object still reachable
  - the dying count not incremented, or not blocking reset/deletion
  - a reaper chunk over `REAP_CHUNK`
  - an object listed twice
  - teardown tied to the deleting thread
- Counters:
  - missed on derive, delete, binding, mapping or unmapping
  - changed by a failed operation or a parentage change
  - overflow not refused
- Freeze:
  - not released when the revoking thread is suspended, destroyed or
    exits, or loses a binding
  - deletion of a revocation's root allowed
  - an enclosing revocation waiting for (instead of cancelling) an
    inner one
  - a second revocation of a frozen object allowed
  - a record inserted by a path other than the two commit functions
  - not set by revocation
  - not checked on one of derive, transfer, binding or retype
  - a carving chain not walked
  - not cleared on abandonment
- Progress:
  - the drain re-examining a failed sender
  - the reaper tearing down an Untyped in a dying CNode in one unit
- Limits:
  - a retype above `RETYPE_ZERO_MAX` accepted
  - serial wraparound reused
  - any chunk constant removed (caught by the work counter)

## Implementation order

Each stage keeps every existing test passing before the next begins,
and each ends with its own gates, mutation testing, review and
documentation. *Revised at stage 2* to the agreed order: dying objects
come before continuations, and the scheduler plumbing waits until
everything it depends on exists.
1. **`struct cap` to 128 bytes and the bracket-list CDT** (SC20, SC20a),
   with the proof's tests (test 7) and gates SC31.1 and SC31.4.
   Revocation switches to first-child deletion, still one-pass in this
   stage: the existing API keeps working until stage 4 replaces it. This
   is the riskiest change, so it goes first, with the scheduler
   untouched.
2. **Accounting: counters, the frame metadata table, the carving
   back-pointers and the creation gates** (SC17-SC19a), checked against
   an independent rescan (gates SC31.2, SC31.3, SC31.5). Remove every
   O(copies) walk. Add the derivation freeze check (SC22a).
3. **Object serials** (SC21), **dying objects and the reaper** (SC23,
   SC24), and the amended O7/O8.
4. **Thread continuations,** then the receive drain (SC25) and revocation
   as a continuation with the freeze (SC22); the retype limit (SC28).
5. **Scheduler plumbing: ownership, transitions and the idle thread**
   (SC1-SC8, SC29), with no timer yet.
6. **Timer preemption, the quantum and fairness** (SC9-SC14), with the
   T36 tests. Then the work counter (SC27) across the whole suite, and
   the amendments above. T36 is then satisfied.

## Revision 3g: stage 6 findings and plan

*Status: proposed.* Stage 6 implements timer preemption, the quantum and
fairness (SC9-SC14), the work counter (SC27) and the T36 tests. An audit
of the accepted text against the code, and of the text against itself,
found seven gaps (H1-H7). H8-H12 are the plan.

**H1. Nobody owns the timer.** SC11 says "nothing else arms it", but no
owner is named, and three places program the LAPIC timer today, all
test code:
- `entry_paths` arms a one-shot to interrupt a spinning user thread
- `sched_halt_wake` arms one as a wake source for the idle thread
- `lapic_timer_stop()` is called by the tests to clean up

The kernel itself never arms it. Once the scheduler arms it at every
dispatch and the idle thread disarms it, both tests break:
- `sched_halt_wake`'s LAPIC case arms the timer before `run_user()`,
  and the idle dispatch then disarms it before the HLT, so nothing
  wakes the CPU and the test hangs
- `entry_paths`'s arming is overwritten at dispatch

*Proposed fix,* one owner per action:

| Action | Owner | Where |
|---|---|---|
| program (arm, re-arm, disarm, set the divider) | `core/sched.c` only, through an arch interface `timer_arm(count)`, `timer_disarm()`, `timer_count()` | arm in `sched_dispatch` of a non-idle thread; disarm in `sched_dispatch` of the idle thread; nowhere else |
| acknowledge (EOI) | the arch interrupt entry, for every vector, exactly once, before any decision | `entry_from_user`, `isr_dispatch` (as today) |
| decide (expiry or stale) | `sched_timer(t)` in `sched.c` | called by the entry path for the timer vector only |

- The SC31-style gate (`scripts/check-sched.sh`) is extended: the timer
  programming functions may be called only from `sched.c`. Tests are
  not exempt here; they go through the scheduler.
- **Test wake sources go through the owner.** The halt/wake test asks
  the scheduler for a wake (`sched_test_wake(ticks)`, test builds only).
  The idle thread arms that one-shot itself in `idle_wait`, right
  before `sti; hlt`, and only when a wake was requested. The self-IPI
  case is unchanged. G4's rule that the escape never stands in for an
  armed halt is unchanged.
- `entry_paths`'s user-mode timer interrupt becomes an ordinary
  quantum expiry, which the T36 tests cover.

**H2. SC11 and SC12 disagree about when the timer is armed, and one
reading breaks T36.**
- SC11 says "every **dispatch** of a user thread ... arms it".
- SC12 says "every dispatch writes a nonzero initial count ...
  immediately before **returning to user mode**".

These are different places. A thread that makes a syscall and stays
RUNNING returns to user mode without a new dispatch.

If arming were placed at the return to user mode (`try_exit_to_user`),
a thread that makes syscalls in a tight loop would get a fresh quantum
on every return and never expire: T36 broken.

*Proposed fix:*
- **Q1, the arming point.** The quantum is armed in `sched_dispatch()`
  when it makes a non-idle thread RUNNING, and only there. A return to
  user mode without a dispatch never touches the timer.
- **Q2, the quantum owner.** In each dispatch, with interrupts disabled
  from `sched_dispatch()` to the return to user mode, `sched_dispatch()`
  arms the timer and records `quantum_owner = t`, per CPU.
  - Every return to user mode asserts `quantum_owner == current`.
  - Every stop, block or yield of the owner clears it.
  - So no path can reach ring 3 without a quantum armed by that
    thread's own dispatch. This is the code-level form of T36's hard
    gate.
- SC12's correctness argument holds unchanged. It needs only that the
  count is written nonzero with interrupts disabled before the thread
  can next take an interrupt, and Q1 satisfies that.

**H3. Quantum accounting has no stated unit or invariant.** SC11 says
"`QUANTUM` ticks" but not what a tick measures, nor whether the quantum
is time, dispatches or work. *Proposed:*
- **Q3, the unit.** A quantum is `QUANTUM` LAPIC timer ticks (at the
  divider `sched.c` sets at boot) of elapsed time from the dispatch. It
  includes kernel time on the thread's behalf: continuation chunks run
  after that dispatch, syscalls, faults and interrupt entries. It does
  not count dispatches, instructions or work units.
- **Q4, the generation.** A quantum belongs to exactly one dispatch.
  Blocking, yielding, stopping or being preempted ends it, and the
  remainder is never carried over. A thread's next dispatch gets a
  fresh full quantum.
- **Q5, expiry.** A timer interrupt preempts if and only if:
  - it was taken from user mode
  - `current` is not the idle thread
  - `current == quantum_owner`
  - the hardware current count reads 0

  Anything else is stale: acknowledged and ignored. An interrupt taken
  in the idle thread never preempts. The count is read through a new
  `timer_count()` (the LAPIC current-count register, x2APIC and xAPIC).
  Nothing reads it today.
- **Preemption uses `sched_yield`.** SC10's `sched_preempt` is not a
  separate primitive. G1's table has one primitive per edge, and
  preemption is the RUNNING → RUNNABLE edge, so the cause is recorded
  only in test counters.

**H4. SC14 mixes units, so it is not provable as written.** The bound
t0 + k·(QUANTUM + W) + W adds QUANTUM, which is in timer ticks, to W,
which is in work units (SC26). A work unit has no fixed duration in
ticks: cache misses, SMIs and emulation make that unknowable. Bounded
kernel work and fairness are different properties, and the text merges
them. *Proposed:* split SC14 into two theorems.
- **SC14a, fairness (by order).** Let X enter the tail of CPU c's run
  queue at t0, with k non-idle threads ahead of it, as SC14 defines k.
  Then at most k dispatches of other threads happen on c before X is
  dispatched.
  - This is purely structural. Every way to become RUNNABLE appends at
    the tail (G1, SC13), dispatch takes only the head, and nothing else
    reorders the queue.
  - It holds whatever the timer does. It is the property that rules out
    starvation by ordering.
- **SC14b, bounded tenure (time).** Each dispatch of a non-idle thread
  holds the CPU for at most QUANTUM ticks plus T(W) ticks: the time
  taken by at most one kernel entry of at most W units in progress when
  the quantum ends (SC9), plus one boundary.
  - QUANTUM is enforced by the hardware timer (Q1-Q5).
  - W is enforced by SC26/SC27.
  - T(W) is a property of the machine, not a constant of the design.
- **Together:** X is dispatched within k·(QUANTUM + T(W)) + T(W) ticks
  of t0. The bound is now explicit about which part comes from the
  scheduler and which from the hardware.
- O(1) primitives (G5) say nothing about starvation. SC14a does, and
  it holds for an O(1) scheduler only because of the FIFO tail rule.
- **Tests:** SC14a is tested exactly, from the dispatch trace, with no
  timing tolerance. SC14b is tested against a measured T(W), with a
  stated margin.

**H5. T36 is a hard gate only if four conditions hold, and two have no
check today.** "No untrusted program runs before timer preemption
exists" needs:
1. **User code always runs with interrupts on.** T13 forces IF on every
   return, user code cannot clear it (IOPL 0), and the LAPIC is not
   reachable from ring 3 (supervisor-only MMIO, MSRs). Holds.
2. **Every return to ring 3 carries a quantum armed by that thread's
   dispatch.** Not checked today; Q2 adds the assertion.
3. **Kernel entries are bounded (W).** Specified (SC26), not checked:
   the SC27 work counter does not exist yet. Stage 6 adds it, as the
   implementation order says.
4. **Only the timer interrupt can end a user thread's tenure
   involuntarily.** The NMI, #DF and #MC take the paranoid path and
   return to the interrupted context; any other vector is acknowledged
   and ignored. Holds.

*Proposed:* T36 is met when the stage 6 tests pass (as ADR 0008 says)
**and** conditions 2 and 3 are assertions in every test build, exercised
across the whole suite: Q2 at every return to ring 3, and SC27 at every
boundary.

**H6. Gap: no interrupt window while the idle thread reaps.** SC9 and SC5
give the idle thread a one-instruction interrupt window between reaper
chunks. The stage 5 code opens interrupts only across the HLT: while the
reaper has work, the idle thread runs chunk after chunk with interrupts
off. The reaper's backlog grows with user activity and has no bound, so
neither does the interrupt latency of an idle-but-reaping CPU. No device
interrupt exists yet, but the self-IPI does.
- *Fix:* the window, `sti; nop; cli`, after each idle reaper chunk, as
  SC5 already says.
- *Test:* a self-IPI pending while the idle thread reaps a 32-slot CNode
  is taken in the window after the first chunk, at a recorded RIP, not
  after the whole drain.

**H7. Gap: today's tests assume no preemption.**
- `run_in_order` and the FIFO and rendezvous suites expect each thread
  to block before the next one starts.
- With a quantum, a thread can be preempted first, and on an emulator
  whether it is depends on host timing.

*Proposed:*
- `QUANTUM` is a constant, and test builds may override it through
  `sched_test_quantum`, which `sched.c` reads at each dispatch.
- The existing suites keep a quantum long enough that their short
  programs never expire. This is checked, not assumed: they assert that
  no preemption happened (a counter), and a suite that did get
  preempted fails rather than passing for an untested reason.
- Test 6 reruns the ADR 0009 suites with a short quantum. It asserts
  only order-independent properties: the audit after every step,
  conservation, and every thread finishing. It never asserts an
  interleaving.

**H8. The timer and continuations stay separate** (E4, G6):
- **The timer only causes a scheduling decision.** It is taken only
  from user mode or the idle thread (SC9). It never interrupts a
  continuation chunk, never creates a continuation boundary, and never
  changes `t->cont`.
- **A thread with a continuation is never preempted by the timer.** It
  does not run user code while its continuation is pending, and each
  chunk already ends at a boundary through `sched_yield` (G6).
- **Each continuation dispatch gets a fresh quantum** (Q1, Q4). If it
  runs out during the final chunk, the pending interrupt is taken at the
  first user instruction after the return, and the thread is preempted
  then. Q3 says that kernel time counts, so this is correct.
- **A mutant** that lets the timer path touch `t->cont`, or skip
  `cont_run`, must be caught.

**H9. Stage 6 plan, in layers, each passing `make test`:**
1. **The arch timer interface and ownership:** `timer_arm`,
   `timer_disarm`, `timer_count` (current-count read). The gate is
   extended (H1), and the two tests are moved to the scheduler's API.
2. **Quantum arming in dispatch,** `quantum_owner`, and the Q2
   assertion at every return to ring 3. Disarming in the idle dispatch.
3. **The expiry path:** EOI, then `sched_timer`, then the Q5 check, then
   `sched_yield`, then the boundary. The idle reaping window (H6).
4. **The SC27 work counter:** every loop whose trip count is not a
   compile-time constant cites its SC26 row and counts. It is asserted
   at every boundary, and the stage 1 pre-scan mutant (SC31.4) is
   finally run against it.
5. **The T36 tests and the H7 reruns.**

**H10. Tests,** the ADR's T36 list (tests 1-6) made exact:
- **SC14a:** two and five threads spinning without syscalls, plus a
  thread looping on syscalls (the H2 case). From the dispatch trace,
  every enqueue-to-dispatch interval has at most k other dispatches,
  and every thread makes progress.
- **SC14b:** T(W) is measured on the TSC during the run, and each tenure
  is at most QUANTUM + T(W) ticks plus a stated margin. The TSC-to-LAPIC
  ratio is calibrated by the test itself, independently of the
  scheduler's bookkeeping.
- **Preemption state:** the preserve-style register and FPU/vector
  checker running under preemption. The thread is at the tail, and it
  returns through IRETQ.
- **Stale interrupts:**
  - an interrupt pending before a dispatch does not preempt the new
    thread
  - one after the quantum ends does
  - one taken in the idle thread does nothing
- **Kernel-mode interrupts outside the idle thread:** zero, apart from
  the test's own window, which is counted separately.
- **The idle reaping window (H6),** and halt and wake through the owner
  (H1).
- **Short-quantum reruns (H7).**

**H11. Mutants** (killed under SC31.6, both as built and with
`sched.c`'s assertions disabled):
- **Timer:**
  - arming at every return to user mode instead of at dispatch (the H2
    mutant)
  - no arming
  - QUANTUM ignored
  - arming left to the idle thread
  - a timer write outside `sched.c` (the gate)
- **Expiry:**
  - preempting on any timer interrupt (no count check)
  - never preempting
  - preempting the idle thread
  - ignoring `quantum_owner`
- **Order:**
  - a preempted thread to the head
  - a woken thread ahead of X
  - the quantum remainder carried over
- **Continuations:** the timer path touching `t->cont`.
- **Work:** an uninstrumented loop (the counter misses it: caught by its
  worst-case test exceeding W), and the SC31.4 pre-scan.
- **Idle:** no window while reaping.

**H12. Non-goals for stage 6:** priorities, SMP, migration (G7 still
holds), tickless accounting, and time-of-day. One level of FIFO round
robin is the whole policy (SC13).

## Revision 3f: stage 5 findings and plan

Stage 5 implements thread ownership, the transitions and the idle
thread (SC1-SC8, SC29), with no timer and no migration. The audit
of the accepted text against the code found five gaps (G1-G5). The
fixes below are accepted; stage 5 implements them.

**G1. SC6's transition table is incomplete.** The code makes transitions
the table does not list:

| Transition | Where today |
|---|---|
| RUNNING → FAULTED | a user fault with no handler (`user_fault`); unsafe saved state found on the way out (`try_exit_to_user`) |
| BLOCKED_SEND/REPLY → FAULTED | a fault call that fails or is answered with an error (`thread_wake` on a fault caller, P71-P72) |
| BLOCKED_SEND → BLOCKED_REPLY | a queued caller taken by a receiver (`ipc_call_link`) |
| RUNNING → INACTIVE by `exit` | `sys_exit` writes the state itself and skips `thread_stop` |

A table with gaps cannot be the single authority. *Proposed fix:* the
complete table below replaces SC6's. Each edge has exactly one
primitive. Any edge not in the table is impossible, and the primitive
asserts its from-state.

| From | To | Primitive | Cause |
|---|---|---|---|
| INACTIVE, FAULTED | RUNNABLE | `sched_start(t)` | resume (SC8) |
| BLOCKED_* | RUNNABLE | `sched_wake(t)` | IPC wake (after IPC has unlinked it) |
| RUNNABLE | RUNNING | `sched_dispatch(cpu)` | the scheduling boundary |
| RUNNING | RUNNABLE | `sched_yield(t)` | `yield`; a continuation chunk not finished (E4); preemption at stage 6 |
| RUNNING | BLOCKED_SEND, BLOCKED_RECV, BLOCKED_NOTIF | `sched_block(t, q)` | IPC wait |
| RUNNING, BLOCKED_SEND | BLOCKED_REPLY | `sched_block_reply(caller)` | a call rendezvous (P58) |
| RUNNING, BLOCKED_SEND, BLOCKED_REPLY | FAULTED | `sched_fault(t)` | no handler, a failed fault call, a fault reply other than label 0, unsafe saved state |
| any but IDLE | INACTIVE | `sched_stop(t)` | suspend, `exit`, destruction, binding loss: the one funnel (abandonment, then IPC detach) |
| (none) | IDLE | at boot only | each CPU's idle thread |

**G2. Five modules write scheduling state.** `t->state`, run-queue
membership and `current` are written in `thread.c`, `user.c`, `cont.c`,
`ipc.c` and `syscall.c`. Nothing stops a sixth.

*Proposed fix:* a new `core/sched.c` holds the primitives above and is
the only writer.
- A build gate, `scripts/check-sched.sh` in `make test`, fails if
  `->state =`, the run-queue links or `->current =` are written
  anywhere else, as SC31 does for the CDT.
- The IPC code keeps the queue and reply-link logic (P9) and calls the
  primitives for every state change.

**G3. `current` is NULL today, which SC2 forbids.**
- When nothing runs, `schedule()` sets `current` to NULL.
- `thread_finalise()` sets it to NULL when a thread destroys itself.
- There is no idle thread, so there is nothing else for it to be.

*Proposed fix:*
- **The idle thread.** Each CPU gets an idle thread: a static TCB in
  per-CPU data, state IDLE, with no binding, no continuation, no reply
  authority and all-zero saved registers. It is not in the
  test-only thread registry, and no capability can name it.
- **`current` is never NULL** after boot. When nothing else runs,
  `current` is the idle thread, and CR3 is the kernel's master page
  table.
- **Self-destruction is an ownership transfer, then destruction.** A
  thread that destroys itself (its last Thread capability deleted in its
  own syscall) is stopped and hands `current` to the idle thread before
  anything in its TCB is torn down: its bindings, the poisoned register
  block in test builds, or the object's memory. No code after the
  handoff touches the old TCB through `current`; the syscall's result is
  written only if `current` is still the caller. A mutant that tears
  down first and hands off after must be caught.
- **Between a block or stop and the next dispatch,** within one kernel
  entry, `current` still names the thread that blocked or stopped. SC1
  and SC2 are checked at scheduling boundaries (SC7), where
  `current` is always RUNNING or IDLE.

**G4. The idle loop cannot halt safely yet, and test builds need a way
out.** SC5's loop ends in `sti; hlt; cli`. Before the stage 6 timer
there is no periodic interrupt. An idle CPU with an empty run queue and
reaper therefore halts until an external interrupt, which is correct
for a release kernel (nothing can become runnable without one). But
it would hang the self-tests. Today the driver regains control by
`schedule()` returning when it has nothing to do. SC5 asks for that
exit to happen "only through the idle thread, so the halt-and-wake path
is always exercised". That second half cannot hold while no wake source
is armed.

*Fix:* two different things, kept apart.
- **The real idle behaviour,** in every build:
  `sti; hlt` → interrupt → kernel entry → the loop runs again.
- **The test-driver escape,** test builds only, is not a halt and does
  not stand in for one. The driver may regain control only at the
  pre-HLT boundary: the run queue and reaper are empty, `current` is
  the idle thread, and no wake source is armed. If a wake source is
  armed, the idle thread always halts and wakes first; the escape can
  never bypass an armed halt-and-wake sequence.
- The halt-and-wake path itself gets a test. It arms a wake source (a
  self-IPI, and the LAPIC one-shot), enters the idle loop, halts, and
  checks that the interrupt arrived in the idle thread's interrupt
  window and the loop resumed.
- At stage 6 the quantum timer makes halting the general case.

**G5. The scheduler's own work has no stated bound.** SC26 bounds what
each syscall's work may cost, but not the scheduler's own operations.
*Fix:* every primitive is O(1). That means the scheduler's own state
change, not everything the operation that called it causes. For
example, `sched_stop` is O(1), but destroying a thread may make objects
dying: that teardown is bounded separately, by the reaper (SC24) and
continuations (SC16).
- **The queue operations** are doubly linked, with no search.
- **`sched_stop`** is `cont_abandon` (O(1), E1) plus `ipc_detach`
  (O(1): it unlinks one queue entry and one reply link, and wakes at
  most one caller).
- **Dispatch** takes the queue head.
- **The idle loop** does one reaper chunk per pass (R5).
- **Nothing is ever destroyed in the scheduler:** CPUs and idle
  threads live forever.
- **There is no wake-all in the scheduler.**
- **The only O(n) walks are audits,** test-only.

**G6. Continuations are not special to the scheduler** (E4).
- The scheduler looks only at `cont.kind != NONE`, in one place:
  dispatch.
- A thread with a continuation is queued and dispatched like any other.
  Its unfinished chunk ends in `sched_yield`, its finished chunk returns
  to user mode, and its cancellation goes through `sched_stop`.
- The capability machinery never touches scheduler structures except
  through those primitives.

**G7. No migration** (SC3).
- `t->home_cpu` is set once at creation (CPU 0) and never written again.
- `sched_start` and `sched_wake` append to the home CPU's queue, and
  dispatch takes only from its own CPU's.
- The run queue and `current` move into per-CPU data. The audit checks
  SC3 for every RUNNABLE and RUNNING thread.

**G8. The audit (SC1-SC4, SC29),** run at boundaries and in every test
audit. It checks:
- **SC1:** each thread's state against what owns it, and exactly once:
  - RUNNABLE: in its home run queue exactly once, and in nothing else
  - RUNNING: the current thread of its home CPU, queued nowhere
  - BLOCKED_*: one wait queue or one reply link, never the run queue
  - INACTIVE, FAULTED: owned by nothing
- **The idle thread:** IDLE, owned only as `current`, never queued, no
  bindings, no continuation, zero saved registers.
- **SC29:** every queue's length is at most the number of live threads,
  with no repeats and symmetric links. This replaces the fixed loop
  limit.
- **Independence.** The expected owner of each thread is derived from
  structures the audit walks itself, not from the scheduler's own
  bookkeeping:
  - it counts each thread's occurrences by walking every run queue and
    wait queue link by link
  - it checks `current` per CPU
  - it then compares with `t->state`, and the queue's stored length with
    the walk

  So a bug in the bookkeeping cannot vouch for itself.

**G9. Tests:**
- every edge of the G1 table, each from-state it refuses, and each
  impossible pair, such as BLOCKED → BLOCKED through `sched_block`,
  INACTIVE → RUNNING, IDLE → BLOCKED, FAULTED → RUNNING, and a destroyed
  thread → RUNNABLE. Each primitive returns `T_EINVAL` for a from-state
  outside its edge. The test checks that the thread's state, run-queue
  membership, queue lengths and `current` are byte-for-byte unchanged.
  Kernel callers assert success.
- `current` never NULL at a boundary, including across self-destruction
  and self-suspension
- the idle thread's halt and wake (G4)
- the idle loop draining the reaper chunk by chunk
- a thread with a continuation queued fairly among others
- SC8's refusals
- `home_cpu` fixed

**G10. Mutants:**
- **Queues:**
  - a duplicate insertion
  - a thread in two queues
  - a stale link
  - removing the wrong thread
  - a queue longer than the live threads
- **Transitions:**
  - waking without enqueueing
  - enqueueing without a state change
  - dispatch not setting `current`
  - blocking leaving the thread queued
  - a stop that skips abandonment
- **Idle and continuations:**
  - the idle thread enqueued or addressable
  - `current` left NULL
  - a continuation given priority
  - `home_cpu` changed
- **Gate:** the build gate must fail on a state write outside
  `sched.c`.
- **Self-destruction:** the teardown ordered before the `current`
  handoff.
- **Test escape:** the escape taken while a wake source is armed.

**G11. One primitive commits each pair.** Every scheduler mutation has
exactly one transition primitive. It commits the state and the
scheduler-owned membership together, so neither can happen without the
other:
- RUNNABLE → RUNNING: removed from the run queue, state set, `current`
  set.
- RUNNING → BLOCKED_*: state set; the thread is no longer runnable. The
  caller has already linked the thread into its IPC wait queue, which
  IPC owns (P9).
- BLOCKED_* → RUNNABLE: state set and appended to the home run queue.
  IPC has already unlinked the thread from its wait queue.
- any → INACTIVE: abandonment, IPC detach, removal from the run queue,
  the state, and the `current` handoff if it is running.

No helper outside `sched.c` changes run-queue membership, so a state
change without its queue change, or the reverse, cannot be written. The
build gate enforces this.

## Revision 3e: stage 4 findings and plan

Planning stage 4 (continuations, the receive drain, revocation with the
freeze, the retype limit) against the code found four places where the
accepted text does not hold as written (E1-E4). The fixes below are
accepted; stage 4 implements them.

**E0. Stage 4's master invariant.** A continuation contains no kernel
object pointer. It contains only immutable values and logical progress
state:
- the captured inputs
- a slot number
- an identity (type, address, serial: D0, D3)
- a cursor

Every chunk resolves its target again and validates its identity before
it touches the target. The slot is only where to look; the identity is
what to find. Every stage 4 operation is an instance of one pattern:

```
capture inputs -> store values -> continuation
  each chunk: resolve the slot -> validate identity, lifecycle, freeze
              -> bounded work -> commit progress
              -> finish (write the result) or keep the continuation
```

A mismatch ends the operation:
- with the error a fresh lookup would give
- or, for a revocation whose root mark has gone, with `T_EINTR` (E1)

It never ends with a wrong object.

**E1. A frozen object has no room to record its owner.** F3 says "the
frozen object records its owner as an internal reference", and F5
cancels a revocation in O(1) through that reference. But D3 filled the
metadata:
- `struct obj_meta` is 32 bytes with no spare word, and growing it does
  not fit a CNode header and an Untyped part in one 128-byte record
- an Untyped's `ut_meta` is 24 bytes with no spare word

*Proposed fix (amends F3, F5, F6):* ownership is recorded on the
revocation's **root record**, not on the object.
- **The mark.** The root record carries a kernel-internal bit
  `CAP_R_REVOKING` in its rights word, above every user-visible right.
  It is never reported (`cap_identify` masks it) and never copied
  (derivation masks it). The object, or for an Untyped the Untyped
  itself, carries `OBJ_FROZEN` as today.
- **The owner** is the one thread whose continuation names that root
  record by (slot, identity). F4 allows one revocation per object, and
  the root cannot be deleted while marked (F5: `cap_delete` returns
  `T_EBUSY`), so the mark, the freeze and the continuation correspond
  one to one. The audit checks that correspondence in both directions.
- **Cancellation (F5)** stays one O(1) step at the record being deleted:
  - an enclosing revocation or the reaper meets a marked root
  - it clears the mark and the freeze
  - it deletes the record

  Nothing is written to the owner. When the owner next runs, its chunk
  re-resolves the root: no marked record with that identity is in that
  slot. That can only mean cancellation, since nothing else removes the
  mark, so the revocation ends with `T_EINTR`. A record later placed in
  the same slot, even another record of the same object, is not marked,
  so it can never be mistaken for the root.
- **Abandonment (F6)** runs in the stop funnel, before the thread can be
  freed or lose its cspace binding (the binding hook runs while the
  binding is intact). It resolves the root the same way, clears the mark
  and the freeze if they are still its own, and discards the
  continuation.

**E1's properties, each enforced and audited:**
1. `CAP_R_REVOKING` is kernel-only.
2. It is never copied by derivation, never transferred by IPC, never
   reported by `cap_identify`, and a user rights mask cannot set it.
3. A marked root is deleted only through the cancellation or abandonment
   funnel: `cap_delete` on it returns `T_EBUSY`. Every other deletion
   path (an enclosing revocation, the reaper) cancels first.
4. An object has at most one marked root record, because F4 refuses a
   second revocation of a frozen object.
5. The continuation holds the root's identity (type, address, serial),
   not only its slot.
6. A chunk resumes only if the record at the slot exists, has that
   identity, **and** is marked; otherwise `T_EINTR`.
7. Cancellation clears the mark and releases the freeze in one step,
   which nothing can observe half-done (the kernel is not preemptible).
   After that step the continuation can never act again: its next
   resolution fails by property 6.
8. A record created later in the same slot is never taken for the root:
   it carries no mark (properties 2 and 6). Its identity also differs,
   unless it is another record of the same object, and then the
   missing mark alone decides.

**E2. A chunked `reply_recv` would let strangers delay someone else's
reply without bound.** SC25 commits `reply_recv`'s reply only after the
drain. One pass drained the senders present at the call. A chunked
drain also meets senders that arrive later: holders of WRITE on the
endpoint can keep queuing undeliverable senders, and the drain never
ends. Meanwhile the *caller* waiting for the reply, a third party,
waits too. That breaks SC14's "no third party is affected".

*Proposed fix (amends SC25):*
- **The drain before the reply is bounded by a snapshot.** The first
  chunk records the number of queued senders, k, as its budget. Before
  the reply commits, the drain fails at most k senders, `DRAIN_CHUNK`
  per chunk, and stops early at a deliverable head or an empty queue.
- Then the reply-half checks (P63 phase 2) and the liveness check run
  again, and the reply commits: atomically, as today.
- If the receive half still has undeliverable senders to fail, it goes
  on as a plain `recv` continuation, which delays only the server
  itself.
- So the caller's wait is bounded by ⌈k/`DRAIN_CHUNK`⌉ of the server's
  chunks, with k fixed when the call began. That is what one pass
  gave, chunked.

**E2's invariant, exactly:** the part of `reply_recv` that can delay the
atomic commit of the reply is bounded by the endpoint's sender queue as
it was when `reply_recv` was invoked. Senders that arrive later belong
to the ordinary receive that follows the commit.
- **k is taken once,** in the first chunk, from the endpoint's own
  queue: the kernel counts the queued senders. Nothing from the caller
  is used, and it is never recomputed.
- **It is carried as a cursor,** `remaining_initial_drain`, which only
  decreases (one per failed sender). The reply commits when the cursor
  reaches 0, the head is deliverable, or the queue is empty, whichever
  comes first.

**E3. Revocation's progress measure ignores stage 3.** SC30 measures a
revocation by the records left in its subtree. Since stage 3, a
revocation can reach an Untyped whose dying objects are still being torn
down (amended O8). It cannot delete that Untyped yet, so the chunk
deletes nothing and the measure does not fall.

*Fix (amends SC30):* the measure is the pair M = (R, B), compared
lexicographically (first R, then B):
- **R:** the records in the revocation's subtree.
- **B:** while the revocation is blocked on an Untyped V (no
  descendants, dying count non-zero), the number of reaper units the
  reaper will still perform before V's dying count reaches 0. Otherwise
  B is 0.

**Proof that M strictly decreases between any two chunks:**
1. **R never grows** while the revocation runs: the freeze admits no new
   record into the subtree (SC22a, F1).
2. **B is a finite natural number.** V has no descendants, so nothing
   new can die from V, and V's dying objects are finished after finitely
   many reaper units (D2 steps 3-6, R7).
3. **A chunk that deletes a record** decreases R. B may then take any
   value: the order is lexicographic.
4. **A chunk that deletes nothing** does so only because the next victim
   is such a V. It ends at once: the thread yields to the tail of the
   run queue, so a scheduling boundary passes before its next chunk.
5. **At every boundary** the reaper performs at least one unit while its
   list is non-empty (R5, `REAP_CHUNK` ≥ 1), and the list is non-empty
   while V's dying count is non-zero. So B falls by at least 1 before
   the next chunk, and R does not grow (step 1).

So no two consecutive chunks leave M unchanged. The sequence of
"revoke chunk, no deletion, no reaper progress, revoke chunk" cannot
occur, and the revocation finishes.

**E4. The dispatch path needs one hook.** SC16 says a dispatched thread
with a continuation runs its next chunk instead of entering user mode.
Today `schedule()` always enters user mode. Stage 4 adds that one branch
there: a dispatched thread with a continuation runs one bounded chunk;
otherwise it enters user mode as before.

The audit rule: a continuation exists only on a RUNNABLE or RUNNING
thread. These combinations are impossible, and the audit checks each:
- BLOCKED (any kind) with a continuation
- INACTIVE with a continuation
- FAULTED with a continuation (a continuation never runs user code, so
  it cannot fault)
- a dying or destroyed thread with a continuation (`thread_stop()` runs
  before a thread's finaliser completes)

Every stop (suspend, exit, destruction, binding loss) goes through the
one funnel, `thread_stop()` → abandonment (E1, F6), before the state
changes. This is the continuation machinery, not stage 5
scheduler work: there are no per-CPU queues, no idle thread and no
timer.

**E5. The continuation record (SC16)** lives in the TCB:
- `kind` (NONE, REVOKE, RECV, REPLY_RECV)
- the slot, and the target's identity (type, object, serial)
- a cursor: for a drain, the senders still allowed before the reply
  commits (E2)
- the captured inputs: the receive offer (`offer`); for `reply_recv`,
  the reply message (`msg`, unused while not sending) and its
  `call_grant` decision

It holds no pointer.

**E6. Revocation (SC22, SC22a, F1-F7):**
- **First chunk.** Look up the slot. Refuse with `T_EBUSY` if the object
  or its carving chain is frozen (F4); otherwise mark the root, freeze
  the object (an Untyped root: freeze the Untyped), and capture the
  identity. All in the syscall's entry, before the thread can be seen as
  RUNNABLE again.
- **Each chunk** deletes at most `REVOKE_CHUNK` first children (or the
  bounded Untyped descent), with no scan. It yields if it meets a pinned
  Untyped (E3).
- **The final chunk** clears the mark and the freeze and returns
  `T_OK`. The syscall's budget argument is ignored.
- **Kernel callers** keep `cap_revoke()` with a step budget. Tests use
  it, and it never runs for a user.

**E7. The receive drain (SC25, E2):** `recv` and `reply_recv`
capture once:
- the endpoint slot and its identity
- the offer
- for `reply_recv`, the reply

Each chunk:
- re-validates the endpoint by identity
- fails at most `DRAIN_CHUNK` undeliverable senders, each exactly once
  (I-E2)
- then completes, blocks, or yields

**E8. The retype limit (SC28):** `RETYPE_ZERO_MAX` is 512 KiB. A larger
zeroed span gets `T_ERANGE` with nothing changed, checked with the
other step-7 arithmetic.

**E9. Tests and mutants:**
- **Revocation:**
  - a subtree of thousands of records (chains and fans), revoked by a
    user thread: exactly `REVOKE_CHUNK` per chunk, the tree equal to a
    reference forest after each
  - many threads deriving into the subtree throughout: refused, and the
    revocation still completes
  - a second revocation of the same object: `T_EBUSY`
  - `cap_delete` of the root: `T_EBUSY`
  - an enclosing revocation, and the reaper, cancelling it: `T_EINTR`
  - the revoker suspended, destroyed, exiting, or losing its cspace
    binding mid-way: the freeze released, `T_EINTR` on resume, deleted
    records stay deleted
  - the audit finds no ownerless freeze and no freezeless continuation
- **The drain:**
  - more than `DRAIN_CHUNK` undeliverable senders: same result as one
    pass, each failed once
  - the offer rewritten in user memory between chunks: no effect
  - the endpoint deleted, and a new one created in the same slot,
    between chunks: the lookup error, never the new object
  - for E2: senders that keep arriving delay the caller's reply by at
    most the snapshot
- **Retype:** over 512 KiB refused; exactly 512 KiB accepted.
- **Mutants** (identity, cursor, ownership, interruption, not only the
  happy path):
  - serial or slot not re-validated
  - user memory re-read after the first chunk
  - a result written before the final chunk
  - mark or freeze not set, not cleared on completion, cancellation or
    any abandonment path
  - the mark copied by derivation or reported to the user
  - a second revocation allowed
  - root deletion allowed
  - an enclosing revocation waiting instead of cancelling
  - the drain failing a sender twice, or ignoring the E2 snapshot
  - `DRAIN_CHUNK` or `REVOKE_CHUNK` ignored
  - the retype limit off by one

**E10. Other loops, checked for a bound.**
- `drain()` and `revoke(c, 0)` are the only unbounded loops reachable
  from a syscall today. Stage 4 replaces both.
- `waitq_wake_all()` has had no caller since stage 3 and is removed.
- `pmm_alloc()` scans linearly, but runs only at boot.
- Retype's per-object initialisation is bounded by `RETYPE_MAX_COUNT`
  and the object sizes, which W counts (SC26).

## Revision 3d: stage 3 findings and plan

Planning stage 3 against the code found three places where revision 3c
does not hold as written (D1-D3). Each fix below is accepted; stage 3
implements them.

**D0. Identity, capability finality and memory lifetime are separate
questions,** each answered by exactly one piece of state. No piece of
state may stand in for another: a zero count is not reuse, a matching
address is not identity, a queue position is not ownership.

| Question | The only authoritative state |
|---|---|
| Is this the same object? | address and serial, compared together (D3) |
| Does any capability to it remain? | its live-record count (SC19a) |
| Can a new capability to it be created? | not dying (R3), and not frozen on its carving chain (SC22a) |
| Can its memory be reused? | its teardown is complete: the reaper's completing unit has dropped its Untyped's dying count (R0, R4, amended O7) |
| Can an Untyped be deleted or reset? | no descendants and a dying count of 0 (amended O7, O8) |
| Can a detached table be installed? | its filled-entry count is 0 (D1) |
| In what order does the reaper work? | its list order, which decides only scheduling, never ownership or lifetime (D2) |

**D1. Address-space teardown is not bounded by `UNIT_MAX`.** SC24 and
SC26 take the most expensive single deletion to be an AddressSpace or
PageTable finaliser "over its 512 entries". The code does more: V15
makes removing a table tear down its **whole installed subtree**
(`subtree_teardown()` recurses), and so do `vspace_uninstall` and the
deletion of an AddressSpace. V15 calls this bounded because the depth is
at most three, but the width is not: one uninstall can visit every
installed table below it, up to 256·512·512 tables' worth of entries.
That is a cost a user chooses, inside one entry, and it would break W.

*Fix (amends ADR 0007 V12 and V15):* removing a table detaches only that
table and never walks below it. **Detaching is not destroying the
subtree.** Everything below a detached table stays exactly as it was,
owned through the same capability relationships: each lower table is
still installed in its parent and each frame still mapped, and their
holders keep every right to unmap, uninstall and delete them. Only
reachability changes: no address-space root leads there any more.

**The invariant (V15', checked by the audit):**
- A populated table that is not installed is inert: it is reachable from
  no AddressSpace and no CPU can translate through it.
- It cannot be installed into any address space.
- It becomes installable only when its filled-entry count is 0.
- `vspace_install()` reads the count itself, in the same non-preemptible
  step that writes the parent entry, before the table becomes reachable.
  It never relies on an earlier check by a caller.

In detail:
- **V15' (replacing V15). A populated table that is not installed is
  inert and cannot be installed.**
  - Uninstalling a table, or deleting its parent, clears one entry and
    flushes the TLB. The table keeps its own entries, but nothing
    reaches them: no CR3 path leads to a table that is not installed.
  - `vspace_install` refuses a table that still has filled entries
    (`T_EBUSY`). Its holder empties it first: `vspace_unmap` and
    `vspace_uninstall` on the capabilities below, each O(1).
  - So V15's purpose holds: nobody can expose a table's frames in
    another address space by moving the table, because a populated
    table never moves.
- **Deleting a table or an AddressSpace** detaches its own entries only:
  at most 512 (a table) or 256 (the user half). The child tables become
  inert and keep their contents; their holders can still unmap, uninstall
  and delete everything below. That is exactly the 512-entry `UNIT_MAX`
  the W table assumed.
- **The cost:** each table gets a count of its filled entries (O(1)
  install check), kept in its frame metadata table entry where a Frame
  keeps its alias counts. A frame mapped in an inert table still counts
  as an alias (SC18), which is conservative: its holder can unmap it.
- *Rejected alternative:* a reaper-driven emptying state for detached
  tables. It needs a reaper link and cursor per table (frame table
  entries grow again) and a new waiting state for install. V15' is
  smaller and needs no kernel-owned work.

**D2. A dying CNode can need an Untyped that cannot be deleted yet.**
The reaper deletes a dying CNode's slots one per unit. When a slot holds
an Untyped U with no descendants, the next step is to delete U. But
amended O8 forbids that while U's **dying count** is non-zero, and U's
dying objects may be listed behind the CNode on the same reaper. Revision
3c says both "teardown never waits for another dying object" (R7) and
"an Untyped with dying objects cannot be deleted". In this case they
contradict each other. Deleting U anyway is not an option: every dying
object's back-pointer names U's record, which would dangle.

*Fix (amends R7):* **requeue, never wait.** If the reaper's next unit
on a dying CNode would delete an Untyped U with no descendants but a
non-zero dying count, the unit instead moves the CNode to the tail of
its reaper list (O(1), counted as a unit). The list order only decides
what the reaper does next. Ownership and lifetime stay with the dying
counts and O8 (D0).

**Termination proof.** It uses the serials of D3: every object,
Untyped included, gets one at creation from a counter that only
increases, so a smaller serial means an earlier creation. Write
X ⇒ Y ("X waits for Y") when a dying CNode X is requeued on an Untyped
U whose record is in one of X's slots, and Y is a dying object carved
from U.

1. **A waited-for object is younger than the waiter.** If X ⇒ Y through
   U, then serial(X) < serial(U) < serial(Y).
   - serial(X) < serial(U): an Untyped capability is never copied (O2)
     and no operation moves a record, so U's only record has been in
     X's slot since U was created. Records in a CNode's slots can be
     created only after the CNode exists: retype, derivation and
     transfer all write into existing slots, and boot install writes
     only into the root cspace, which is not a CNode object.
   - serial(U) < serial(Y): Y was carved from U by a retype that used
     U's capability, so U already existed.
2. **⇒ has no cycles.** Serials strictly increase along every ⇒ chain,
   so a chain can never return to where it started.
3. **The set of objects X can wait for is fixed and finite once X is
   dying.** No capability refers to X, so nothing but the reaper reaches
   X's slots. So no retype can use an Untyped whose record is in X, and
   everything ever to be carved from it already exists. The same holds
   for every dying CNode further down ⇒. So the closure Dep*(X), all
   objects reachable from X along ⇒, is a fixed finite set. Its total
   remaining teardown work is also finite: no capability refers to a
   dying object, so no waiter or record is ever added to one (R3, R7).
4. **The dependency set only shrinks.** While X is requeued on U, U has
   no descendants: nothing carved from U is live, so nothing new can die
   from U, and U's dying count only falls (each fall is a teardown
   completing).
5. **At least one unit of real work separates any two requeues of X.**
   Take, among the unfinished objects of Dep*(X), the one with the
   largest serial, M. Everything M could wait for has a larger serial
   (step 1). So it is finished, or not in Dep*(X), and M never requeues.
   After X's requeue every other listed object is ahead of X, M
   included. So before X is reached again, the reaper reaches M and does
   at least one unit of M's real teardown (a waiter woken or a slot
   deleted).
6. **X finishes.** By steps 3 and 5, X is requeued at most as many times
   as there are units of real work in Dep*(X), a finite number. Once its
   dependencies are finished, U's dying count is 0 (step 4) and X
   proceeds. Every requeue is one O(1) unit at a scheduling boundary, so
   R5 still holds.
7. **The delay is delegated work.** Everything X waits for was carved
   from Untypeds that X's own slots held. That is work its holders chose
   to create (R8).

The same situation at user level needs no change: `cap_delete` on such
an Untyped returns `T_EBUSY`, a plain retryable error.

**D3. Serials are needed on every object type, and the record layout
must make room.** SC21 gives serials to endpoints, notifications and
CNodes. But SC16 and F6 identify a revocation's target by (address,
serial), and a revocation can target a record of any type. So every
object with metadata needs a serial.
- **The rule:** every kernel object that capabilities can name has a
  serial that is never reused, and identity has one predicate for every
  type, `same_object(a, b)`: same address **and** same serial. No
  caller needs to know which types carry serials, because all of them
  do.
- `struct obj_meta` gains the 64-bit serial and becomes 32 bytes.
  - The frame metadata table grows to 32 bytes per page: 2.0 MiB at
    256 MiB of RAM (0.8%), up from 1.5 MiB.
  - Endpoints, notifications and threads have room in the object.
- **The 128-byte record still fits** (SC20a), with no loss of
  capability bits. The 56 bytes after the fixed fields hold:
  - **a CNode header** (slot 0's slot property, 32 bytes): live count,
    flags, carver, serial, and the reaper link, which replaces both the
    rights-count union (a CNode has no rights counts) and
    `pending_link`.
  - **an Untyped part** (the record's own, 24 bytes): flags, dying
    count, carver and serial. There is no live count: an Untyped has
    exactly one record (O2), so the record's existence is its count.
  - The CNode's reaper cursor uses the 4 bytes after `slot_flags`
    (today `reserved0`), which become a slot property.

**D4. The death transaction (R0) as stage 3 implements it.** It is all in
`destroy_slot()`, in one non-preemptible step, in this order:
1. the count goes from 1 to 0
2. for a type whose teardown is not O(1) (an endpoint or notification
   with waiters, a CNode with occupied slots): mark it dying, read its
   carver, increment the carver's dying count, and append it to the
   reaper
3. unlink the record and clear its slot

The object's memory is reusable only after the reaper's completing
unit decrements the dying count, never when the count reaches 0.
Everything else keeps its finaliser, now bounded by D1:
- a Thread: stop it and delete its four bindings
- an AddressSpace or PageTable: at most 512 entries
- a Frame, an Untyped or a test token: nothing

An endpoint or notification with **no** waiters needs no teardown and
is finalised at once. Authority loss (the last READ or WRITE record of a
live endpoint, with waiters in the doomed direction) lists the live
object with the doomed direction instead of waking every waiter in the
deleter's entry, as the code does today.

**D5. Where the reaper runs before stage 5.** There is no idle thread
and no per-CPU scheduler yet. Stage 3 runs `REAP_CHUNK` units at the one
scheduling boundary that exists, the top of each pass of `schedule()`,
and exposes a test-only call to run units directly. There is one CPU,
so one reaper list. Stage 5 moves the call into the per-CPU idle loop
and boundaries. This is a single call site, not scheduler work.

**D6. Identity rules made executable:**
- **Serials are never reused:** one global 64-bit counter that only
  increases. A retype of n objects first checks that n serials remain,
  else `T_ENOMEM` with nothing changed (test 13, with a test-only hook
  to put the counter near exhaustion).
- **The reaper's references are pinned internal references,** like
  wait-queue links (O10): the dying count pins the memory (R4), so the
  reaper never needs a value identity. The audit cross-checks every
  listed object's serial against a copy taken when it was listed.
- **Continuations (stage 4) hold (slot, address, serial).** Stage 3 adds
  the one comparison function they will use, so the ABA tests can
  exercise it now.

**D7. The ABA lifecycle tests.**
- **During A's death:**
  - A (in slot X, serial S1) dies with more than `REAP_CHUNK` waiters
    and becomes dying.
  - B is retyped into slot X: same slot, necessarily a different
    address, since A's memory is pinned.
  - While A's reaper work is pending:
    - an identity check of (X, A's address, S1) fails
    - operations on B work: derive, transfer, delete
    - a retype that would reset A's Untyped gets `T_EAGAIN` with
      nothing changed
    - deleting that Untyped gets `T_EBUSY`
  - The interleaving is varied: A dies, B appears, part of the reaper
    runs, B derives, B is deleted, the reaper continues. A's waiters
    are each woken exactly once, and B's records and waiters are never
    touched.
- **After A's teardown:**
  - the memory is reset and C is created at A's address in slot X, with
    serial S3 > S1
  - (X, A's address, S1) still fails; only S3 matches
- **Mutants:**
  - the dying count not incremented, which lets C appear while A's
    reaper work is pending: the test must catch the reaper touching C
  - the serial not compared
  - a serial reused
  - the requeue of D2 replaced by waiting, or by deleting U early
  - `vspace_install()` not reading the filled-entry count, or a filled
    count not maintained on map, unmap, install or uninstall (D1)
  - a detach that walks below the detached table (caught by the work
    bound once SC27 exists, and before that by a test that the lower
    tables keep their entries)

**D8. Audits added in stage 3 (the oracle stays independent).**
- For every Untyped, the dying count equals the number of listed dying
  objects whose back-pointer names it. The oracle counts these from the
  reaper list, never from the Untyped.
- Every listed object is dying, or live with a doomed direction, and is
  listed once (R1, R2).
- No capability record anywhere refers to a dying object (R3), checked by
  the storage scan of stage 2.
- Serials are strictly increasing in creation order, and unique among
  live and dying objects.
- An inert table (D1) is not installed and is reachable from no
  AddressSpace.

**D9. Cost accounting.** Fixed kernel memory, recorded in
`ARCHITECTURE.md`: the frame metadata table (1.5 MiB at 256 MiB at
stage 2; 2.0 MiB from stage 3, with D3). Endpoints and notifications are 128 bytes.

## Implementation notes

**Stage 1 (done).**
- `struct cap` is 128 bytes and 128-byte aligned. The CDT is a
  balanced-bracket list; every CDT function in `cap.c` names the proof
  step of SC20 it implements (gate SC31.1).
- Revocation deletes the first child each step, with no subtree scan
  (gate SC31.4). It never deletes an Untyped that still has descendants:
  the victim search descends through such Untypeds (at most `CHAIN_MAX`
  of them), so O8 holds at every step and the carving back-pointers of
  stage 2 stay valid. The revoke API is unchanged until stage 4 (stage 5 in the original numbering).
- **Transitional:** `is_final()`, `cap_object_first()` and
  `cap_object_next()` still decide finality and iterate an object's
  records by list position, adapted to brackets. That is correct under
  today's rules, but SC19a forbids relying on it. Stage 2 replaces them
  with the live-record count, and gate SC31.2's build check then forbids
  them.
- **Tests:**
  - every existing CDT, revocation, Untyped and transfer-conservation
    test now checks parentage, never depth or list order
  - a reference forest checks the A{B{C{D},E}} deletion orders, the
    revision-2 counterexample (and that revoking w leaves z alone),
    re-derivation into the same slots across cspaces, and 1,600 random
    derive, delete and one-step-revoke operations over two cspaces and
    several objects, including the finaliser firing exactly at the last
    record
  - an Untyped descent test asserts the exact deletion order and step
    count
  - a 3,071-record subtree (a chain and a fan) is revoked one record per
    call, at exactly one step each
- **Mutation testing:** 11 mutants, 10 caught. The equivalent one
  deletes the last child instead of the first: any child is a correct
  revocation victim, and both are O(1). SC22 names the first child only
  to fix one choice.
- **Records on the stack:** doubling the record size pushed a test
  frame holding eight records over the 1 KiB frame limit (T33); test
  storage for records is now static. No kernel function keeps a record
  on its stack.

**Stage 2 (done).**
- **Where the metadata lives** (`kernel/core/objmeta.c`): a 24-byte
  `struct obj_meta` (live count; READ and WRITE counts, or for a Frame
  the writable and executable alias counts; freeze flag; carving
  back-pointer) at the SC19a location for each type. Endpoints and
  notifications grew from 64 to 128 bytes (`ENDPOINT_BITS` 7) to hold it.
  A CNode's is the slot property `slot_meta` of its slot 0, which
  `cap_slot_clear()` preserves; an Untyped's is `ut_meta` in its own
  record. `struct cap` stays 128 bytes: the six reserved words became
  these two fields.
- **The frame table** is allocated by `pmm_alloc_contig()` in
  `untyped_boot()` before the first Untyped, counted as kernel frames,
  and never freed (256 MiB of RAM: 1.5 MiB of table). Boot panics if it
  does not fit.
- **Finality** is `meta_del()` taking the count from 1 to 0, in
  `destroy_slot()` between the cleanup hook and the unlinking. Cleanup
  hooks therefore see counts that still include the record being
  deleted (an endpoint's last READ record is the one with READ when the
  count is 1). `is_final()`, `cap_object_first()`, `cap_object_next()`,
  `cap_same_object()`, the endpoint `count_with()` walk and the
  notification `count_write()` walk are gone, and V6's alias check is
  O(1) from the alias counts.
- **Count 0 is not reuse.** Reaching 0 clears the metadata (nothing stale
  survives into the memory's next object, and retype asserts it finds
  zero) and runs the finaliser synchronously, as before. Whether memory
  can be reused is still decided only by O7 (the Untyped has no
  children); stage 3 inserts the dying transaction between the count
  reaching 0 and that point, and will read the carving back-pointer at
  the dying transition rather than after it.
- **Counts change only in commit steps:** `cap_derive_commit()` (derive,
  transfer, binding), retype step 9, `cap_install()` (boot roots),
  `destroy_slot()`, and for aliases `vspace_map()`, `cap_detach()` (unmap,
  deletion, page-table teardown), `vspace_protect()` and the IPC-buffer
  binding. A failing operation returns before any of them (C5).
- **Creation gates.** `cap_install()` now accepts only the types no retype
  can make (boot Untypeds, the debug console, test tokens), so every
  other object has a carver. `cap_derive_check()` ends with
  `meta_check_new()`: `T_EBUSY` if the source's object or any Untyped on
  its carving chain is frozen, `T_ERANGE` if the count has no room
  (`OBJ_LIVE_MAX` leaves room for the three records one message can
  create). Retype refuses a frozen chain with `T_EBUSY` at its step 5a.
  Until stage 4 nothing in the kernel sets the freeze flag; tests set it
  through a test-only hook.
- **Gate scripts** (`scripts/check-cdt.sh`, run by `make test`): SC31.2
  forbids the positional helpers, any object comparison involving a CDT
  neighbour, and any object comparison in `cap.c`; SC31.5 requires
  `cap_insert_child()` to be called from exactly `cap_derive_commit()`
  and `cap_retype()`. Each check was shown to fail on an injected
  violation.
- **The independent oracle** (`meta_audit()` in the self-tests) finds
  records by scanning storage (the root cspace, every test store, every
  live thread's binding slots, every CNode reachable from those), never
  through a CDT link. It reads frame mappings from the hardware entries,
  not from `map_state`, and locates stored metadata itself rather than
  through `obj_meta()`. It compares, for every object: the live count,
  the rights or alias counts, that an Untyped has one record, and that
  the carver is a live Untyped containing the object. It also checks
  that no metadata exists for an object without records: frame table
  entries, test-token entries and registered threads. It runs after
  every test, inside every IPC audit (`AUDIT()`, `ep_ok()`, `notif_ok()`)
  and at each step of the new tests.
- **A test bug it found:** the random CDT test re-initialised its two
  cspaces between rounds without deleting their live records. That left
  the previous round's objects counted (and their CDT links dangling
  into zeroed memory). Positional finality could not see it; the
  counter did.
- **Tests:** `meta_counts` (every location, failure atomicity, finality
  regardless of which record goes last, re-parenting changes no count),
  `meta_frames` (map, same-rights and narrowing protect, unmap,
  deletion, page-table teardown, failed maps), `meta_overflow` (derive,
  binding and transfer refused with `T_ERANGE`, nothing changed, the
  limit itself allowed), `meta_freeze` (the object, one and two carving
  links, the middle of a chain, unrelated objects unaffected, deletion
  still allowed, a refused transfer leaves sender and receiver
  byte-identical).
- **Mutation testing:** 26 mutants, all caught. There is one per counter
  site (derive, install, retype, delete, binding add and remove, map,
  unmap, both halves of protect, READ increment, WRITE decrement), four
  wrong-object updates (the first object of a retype, the next frame
  table entry, slot 1 of a CNode, the source instead of the copy), the
  four freeze bypasses (derive, retype, chain not walked, one link
  only), overflow not refused, positional finality, an alias counted
  before a failing map, the final metadata not cleared, and both
  liveness thresholds. The one survivor of the first run (protect's
  increment, a no-op unless a mapping is re-protected to the same
  rights) got a test. All 18 counter mutants were then re-run with every
  assertion in the metadata code compiled out, so that only the tests
  could catch them: all 18 were caught, most by the oracle.

**Stage 3 (done).** Revision 3d, D1-D8, in three layers, each passing
the whole suite.
- **Serials (D3, SC21).** `struct obj_meta` is 32 bytes and carries a
  serial; an Untyped's metadata is a 24-byte `struct ut_meta` in its own
  record (no live count, O2). One counter issues every serial.
  - It only increases. Retype checks at its new step 5b that enough
    serials remain, otherwise `T_ENOMEM` with nothing changed.
  - `obj_identity_is()` is the single identity predicate: type, address
    and serial together.
  - The frame table is now 2,044 KiB at 256 MiB (measured at boot).
  - The record still fits in 128 bytes: `pending_link` became the CNode
    header's reaper link (in the metadata union), and `reserved0` became
    the slot property `slot_aux` (the reaper's cursor).
- **Bounded detach (D1, V15').**
  - Every fill and clear of a hardware entry goes through `entry_fill()`
    and `entry_clear()`, which keep the table's filled-entry count in
    its frame table entry.
  - `vspace_uninstall` clears one entry.
  - Deleting a PageTable, or an AddressSpace, detaches only its own
    entries.
  - `vspace_install()` reads the count at the reachability commit and
    refuses a populated table.
  - Metadata is now cleared by `meta_retire()` once teardown completes,
    never when the count reaches 0, so an AddressSpace finaliser still
    sees its own counts.
- **Dying objects and the reaper (SC23, SC24, D2, D4, D5).**
  - **The death transaction.** `destroy_slot()` runs it in R0's order:
    count to 0, then `reaper_take()` (mark dying, read the carver, raise
    its dying count, list), then unlink the record and clear the slot.
  - **What dies through the reaper:** every CNode, and an endpoint or
    notification that has waiters or is already listed. Everything else
    is finalised at once, within `UNIT_MAX`. "Every CNode", not only
    occupied ones, avoids a scan in the deleter's entry; an empty CNode
    costs the reaper one unit.
  - **Authority loss** lists a live object with its doomed direction
    (`OBJ_DOOM_SEND`, `OBJ_DOOM_RECV`) instead of waking every waiter in
    the deleter's entry.
  - **One reaper unit** is one of: one waiter failed with `T_EDEAD`;
    one CNode slot deleted; for an Untyped in a dying CNode, one
    descendant deleted (the Untyped frozen by the reaper, F7); or a
    requeue (D2). Its completing unit unlists the object, drops the
    dying count and retires the metadata, together.
  - **Where it runs** (until stage 5): at the top of each pass of
    `schedule()`, `REAP_CHUNK` (4) units at a time. With nothing
    runnable the loop keeps reaping, one chunk per pass: the idle loop
    of D5.
  - **The old drain is gone.** `cnode_finalise()` and its pending list
    are removed; T33's constant stack now comes from the reaper's flat
    list.
  - **Amended O7/O8.** The watermark is not reset while the dying count
    is non-zero; a retype that then does not fit gets `T_EAGAIN`.
    Deleting such an Untyped gets `T_EBUSY`, and a revocation that
    reaches one gets `T_EAGAIN`.
  - **D2 at a deeper level.** The victim search can also stop at a
    *nested* Untyped with dying objects. That blocks too and requeues,
    and D2's proof covers it: everything carved from the nested
    Untyped is younger still.
- **Audits.** The oracle now also checks:
  - every object's serial: issued, unique, and greater than its
    carver's
  - each table's filled count against its hardware entries
  - R3: no object with a record is dying; only a live endpoint or
    notification may be listed, and only with a doomed direction
  - the reaper list itself: each entry listed once and flagged as
    listed; each dying entry has no records, has a serial, and was
    carved from a live Untyped
  - each Untyped's dying count against the dying entries naming it,
    counted from the list and never read from the kernel's tally
- **Tests:**
  - `reaper_dying`: the R0 state after the death; `T_EAGAIN` and
    `T_EBUSY` with byte-identical state while pinned; exactly
    `REAP_CHUNK` waiters per boundary, each failed once; the memory
    reset only afterwards
  - `reaper_aba`: the two-phase D7 test (below)
  - `reaper_requeue`: D2's proof at run time, from a trace of every
    unit. It checks: the requeue happens; only strictly younger objects
    are worked on; real work separates requeues; Y1-Y3 finish before X,
    which finishes last; and F7's freeze refuses derivation from a live
    descendant mid-teardown.
  - `serial_exhaustion`
  - `vspace_teardown`, rewritten for V15': detach is not destruction,
    and a populated table is installable only after its holders empty
    it level by level
  - existing liveness tests now check both halves: still pending after
    the deletion, done after the reaper runs
- **A test that first proved too little.** The first ABA test passed
  even with the pin removed: B came from the same Untyped as A and was
  alive, and a live child alone blocks a reset. Now B comes from
  another Untyped, so only A's pin keeps D off A's address. With the
  pin removed and the oracle switched off, the test fails on its own:
  D lands at A's address, A's reaper corrupts it, and the kernel
  faults.
- **Mutation testing:** 24 stage-3 mutants, all caught (see the stage 3
  commit message for the list).
- **Not tested directly:** R6 (stopping the deleting thread changes
  nothing). It holds by construction: the death commits in the
  deleter's entry, and the reaper refers to no thread.

**Stage 4 (done).** Revision 3e, E0-E10, in two layers.
- **Continuations** (`core/cont.c`, `struct thread_cont` in the TCB).
  - **What they hold:** values only: the kind, the slot, the target's
    identity (type, object, serial), and a cursor (`aux`). The captured
    offer and reply live in the TCB's `offer` and `msg`. No pointer
    survives between chunks; even the transfer plan is rebuilt in each.
  - **How they run:** the first chunk runs in the syscall. An unfinished
    operation yields, RUNNABLE, to the tail. `schedule()` runs the next
    chunk instead of entering user mode (E4), and only the final chunk
    writes RAX.
  - **How they stop:** every stop goes through `thread_stop()` →
    `cont_abandon()` first. That releases what the continuation holds
    and leaves `T_EINTR`, unless the thread is being destroyed.
  - **IPC audit rule 12:** a continuation only on a RUNNABLE or RUNNING
    thread, of a known kind, with no residue. Rules 7 and 8 let a drain
    continuation hold its offer and captured reply.
- **Revocation** (`SYS_CAP_REVOKE`):
  - **The root and the freeze.** The root gets the kernel-only
    `CAP_R_REVOKING` mark. It is masked from `cap_identify`, masked in
    derivation, and refused by `cap_delete`. The object, or for an
    Untyped root the Untyped itself, is frozen.
  - **Each chunk** re-resolves the root by slot, identity and mark, and
    deletes at most `REVOKE_CHUNK` (8) first children. It yields on an
    Untyped pinned by dying objects (E3).
  - **Cancellation.** `destroy_slot()` and `cap_reap_slot()` cancel a
    marked root before deleting it: an enclosing revocation, the reaper,
    or a finaliser. The owner then finds no marked root with its
    identity and ends with `T_EINTR`.
  - **The kernel API** `cap_revoke()` keeps its step budget for tests,
    and refuses a marked root.
  - **The audit** checks the one-to-one correspondence of marked root,
    frozen object and revocation continuation, from both sides.
- **The receive drain** (`recv`, `reply_recv`):
  - **Capture once.** The slot and identity, the offer and, for
    `reply_recv`, the reply (`reply_capture()`, which reads user memory
    once), and k from the endpoint's own `senders.len`. Wait queues now
    keep their length; the audit checks it.
  - **Each chunk** re-resolves the endpoint. A different endpoint in
    the slot gets `T_EBADCAP`. It fails at most `DRAIN_CHUNK` (8)
    senders, each exactly once.
  - **`reply_recv`** commits its reply after at most k failures, with
    `reply_validate()` re-run on the captured copy, then continues as a
    plain receive (E2).
- **The retype limit:** `RETYPE_ZERO_MAX` is 512 KiB; Untyped children
  are exempt (SC28).
- **Unbounded loops:** `waitq_wake_all()` is now test-only. `drain()`
  and the unbudgeted revocation reachable from a syscall are gone.
- **Tests:**
  - `revoke_chunks`: a 3,071-record subtree, exactly `REVOKE_CHUNK` per
    chunk, against the reference forest. Also the F4 and F5 refusals,
    the mark never shown, abandonment, and E1 properties 5 and 8: a
    cancelled continuation never adopts another revocation's root,
    even in its own slot.
  - `revoke_cancel`: an enclosing revocation, and the reaper.
  - `revoke_syscall`: user threads; derivers refused throughout; and
    suspend, cspace unbind and destruction mid-way, each releasing the
    freeze and leaving `T_EINTR`.
  - `revoke_blocked`: E3's measure, step by step.
  - `revoke_no_early_result`: RAX untouched before the final chunk.
  - `drain_chunks`: two chunks; each sender failed once; the offer
    rewritten in user memory between chunks has no effect.
  - `drain_identity`: the endpoint deleted, or replaced in its slot,
    between chunks.
  - `drain_reply_bound`: senders that re-send after every failure delay
    the reply by exactly the k senders of the snapshot. It uses a new
    `resend` user program, and senders that share a task at two slots
    each.
  - `retype_limit`, at the exact boundary.
  - `reaper_r6`: R6 tested directly. The deleter stops, or destroys,
    itself in its next syscall; the reaper had done no unit when it
    stopped, and the teardown is exact.
- **Mutation testing:** 24 stage-4 mutants, aimed at identity, cursor,
  ownership and interruption. 23 are caught. One, `mark_copied`, is
  **equivalent under the current state machine**, not untestable:
  - a marked root's object is always frozen (E1)
  - derivation from a frozen object is refused before the commit
  - so the mark can never reach a copied record, and masking it in
    `cap_derive_commit()` cannot be observed

  The mask stays as defence in depth. If a future operation can derive
  or copy from a frozen root, the equivalence ends, and that change must
  make this mutant killable with a test.
  - **A survivor that taught something:** the first run let
    `rev_no_identity` (resume on the mark alone) survive. A cancelled
    continuation could adopt a later revocation's root in its own slot,
    giving two owners for one freeze. The test above now catches it.
  - The stage 2 and stage 3 sets were re-run against stage 4.

**Stage 5 (done).** Revision 3f, G1-G11, in the order agreed at
acceptance: primitives, idle and `current`, run queue, transition
enforcement, dispatch and continuations, self-destruction, halt and
wake, then the transition tests and the mutation audit.
- **`core/sched.c`** holds the per-CPU run queue (FIFO, doubly linked
  through the TCB), the idle thread and the SC6 primitives. It is the
  only writer of `t->state`, the run-queue links, `home_cpu` and
  `current`.
  - Each primitive checks its from-state and the links its edge needs.
    `sched_wake` and `sched_fault` need IPC to have unlinked the thread
    first, `sched_block` needs it to be linked into a queue of that kind,
    and `sched_block_reply` needs the reply link. Any other case returns
    `T_EINVAL` with nothing changed. Kernel callers assert `T_OK`.
  - `sched_start` also refuses a thread that is not runnable-safe (T4).
    `thread_resume` keeps its `T_EPERM` for that case, so the primitive
    refusal matters only to a kernel caller, and to a destroyed thread,
    whose bindings are gone.
  - `sched_stop` is the one funnel: `cont_abandon`, `ipc_detach`, off
    the run queue, INACTIVE. `exit` now goes through it too.
- **The gate** `scripts/check-sched.sh` (in `make test`) fails on an
  assignment, a compound assignment, `++`/`--` or a taken address of
  those fields outside `sched.c` and the self-test file. It was shown to
  catch 13 write forms and to pass reads.
- **The idle thread** is a static TCB per CPU: zero except its state
  (IDLE) and home CPU, never registered, never queued, never named by a
  capability. `sched_boot()` makes it `current`, so `current` is never
  NULL after boot.
  - `schedule()` asserts at every boundary that `current` is what was
    dispatched, and that it is RUNNING or IDLE.
  - A self-destroying thread is stopped, then `sched_release_current()`
    hands `current` to idle, and only then does `tcb_teardown()` delete
    its bindings. The syscall result is written only if `current` is
    still the caller.
- **The idle loop** (G4) does one reaper chunk per pass and, with the
  reaper empty, halts with `sti; hlt; cli`.
  - In test builds it may instead return to the driver, but only at
    that pre-HLT boundary: the run queue and reaper are empty and
    `current` is idle (asserted), and no wake source is armed.
  - Armed, it always halts. `idle_hlt_resume` labels the instruction
    after the HLT, and the kernel interrupt path records the interrupted
    RIP, the vector and whether idle was `current`.
- **The audit** (G8) adds to `ipc_audit()`:
  - each thread's run-queue occurrences, counted link by link, are 1 for
    RUNNABLE and 0 otherwise
  - RUNNING exactly when `current`; no stale run-queue link
  - each blocked thread exactly once in its wait queue
  - the run queue well formed, its stored length equal to the walk
  - every queue no longer than the live threads (SC29, replacing the
    fixed loop limit)
  - `home_cpu` valid and, for RUNNABLE and RUNNING, this CPU
  - the idle thread byte-clean but for its state and home
- **Tests changed by the stage:**
  - `ipc_reply_links` built its callers by linking stopped threads, an
    INACTIVE → BLOCKED_REPLY edge that the table does not have. Callers
    are now started and dispatched first (`link_reply()`).
  - `reaper_r6` read "the last stop of the run". Since `exit` goes
    through the funnel, that is a waiter's exit, so the probe now
    watches the deleter itself (`sched_stop_watch`). A first version
    read the deleter's TCB pointer after its capability was gone and
    watched nothing: the probe caught that by never firing.
  - The snapshot helper walks the run queue read-only instead of
    popping and re-pushing it.
- **New tests:**
  - `sched_edges`: a fixture with a thread in every state. Every
    primitive is run against every thread, the idle thread included.
    Each of the 87 refusals leaves every fixture TCB, the run queue's
    ends and length, `current` and the IPC queues byte-for-byte
    unchanged. The same holds for `current` while blocked or stopped
    before its boundary, and for queue kinds that are not wait-queue
    kinds. Then every edge, with its membership checked. Then SC8, a
    destroyed thread, and `home_cpu`.
  - `sched_current`: self-suspension, self-destruction (exactly one
    handoff, no teardown while `current`), and outside destruction (no
    handoff).
  - `sched_halt_wake`: unarmed, no halt. Armed, a self-IPI pending
    before the HLT and a LAPIC one-shot after it: one halt, one wake,
    the interrupt at `idle_hlt_resume` in the idle thread.
  - `sched_idle_reap`: a dying 32-slot CNode drained by the idle loop
    in full `REAP_CHUNK` passes and no more.
  - `sched_fair_cont`: a 64-child revocation and two yielding threads.
    The dispatch trace is strict round robin up to the revocation's last
    chunk.
  - `sched_audit_detects`: ten corruptions, each noticed.
- **Mutation testing (G10):** 31 stage 5 mutants, run twice: once as built, and once with every
  assertion in `sched.c` and the boundary and escape assertions in
  `schedule()` disabled, so that only the tests and the audit can catch
  a mutant. All 31 are caught both times.
  - **The mutants** cover the queues (duplicate insert, a blocked or
    stopped thread left queued, a stale link, the wrong thread removed,
    dispatch from the tail, a length not decremented), and the
    transitions (wake without enqueue, start without a state change,
    dispatch without `current`, a stop that skips abandonment or detach,
    each precondition check dropped).
  - They also cover idle and continuations (idle stoppable, enqueued,
    registered or dirty; `current` left NULL; a continuation given
    priority; `home_cpu` changed), a state write that only the gate can
    see, teardown before the handoff and no handoff, the escape taken
    while armed, an unbounded reaper pass, and the idle thread halting
    with reaper work left.
  - **Two survivors taught something.** `block_any_kind` (no check that
    the kind is a wait-queue kind) and `yield_any_state` (yield checks
    only `current`) survived the first run. The fixture had no thread
    that was `current` without being RUNNING, and none linked to a
    queue of the wrong kind. `sched_edges` now covers both.
  - **One false kill:** `escape_while_armed` first "died" because the
    compiler removed the unreachable HLT and its label, so the link
    failed. It was rewritten to stay reachable, and the halt/wake test
    kills it. The harness now counts a mutant as killed only if the
    suite ran.
  - The stage 2, 3 and 4 sets were re-run against stage 5: 73 caught.
    `mark_copied` is still equivalent (stage 4 notes). Six stage 2
    patterns had already been obsolete since stage 3. `no_abandon_funnel`
    was retargeted to `sched_stop`, where the funnel now lives.
- **Review:**
  - The new TCB fields (`home_cpu`, `sched_reserved`) fill what would
    otherwise be padding and are zeroed by retype. Every snapshot or view
    struct has explicit padding and is cleared before it is filled.
  - The idle TCB is static and cleared at boot. Test probes holding
    thread pointers compare them only by value and are reset after use.
  - All new frames are under the 1 KiB limit. The suite's measured
    high-water marks after stage 5: boot stack 1,560 of 16,384 bytes,
    CPU 0 kernel stack 1,320 of 8,192. An interrupt taken in the idle
    loop runs on the stack `schedule()` runs on, within those marks.
  - The release build is checked separately: `make test` builds only
    the test kernel, and a test-only variable broke the release build
    once during the stage.
