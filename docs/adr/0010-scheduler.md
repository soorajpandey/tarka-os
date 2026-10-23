# ADR 0010: Scheduler, timer preemption and bounded kernel work

Status: accepted (revision 3c, `2c517b1`; step 6). Rule numbers SC1..SC32 are
referenced from code, tests and other ADRs.

History:
- **Revision 1** (`5502c54`): first draft.
- **Revision 2** (`1fd873c`): idle thread, continuations, counters, W.
- **Revision 3c** (this one) specifies the freeze lifecycle (SC22b): the
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

**SC6. One primitive per transition.** Thread state, run-queue membership
and each CPU's `current` change only through these functions, each
updating all three together and asserting its precondition:

| Primitive | From | To | Effect |
|---|---|---|---|
| `sched_make_runnable(t)` | INACTIVE, FAULTED, or BLOCKED (already unlinked by IPC) | RUNNABLE | appended to the home run queue |
| `sched_dispatch(cpu)` | the queue head, or the idle thread if the queue is empty | RUNNING (or IDLE) | removed from the queue; becomes `cpu->current`; a user thread gets a fresh quantum (SC11) |
| `sched_preempt(t)` | RUNNING | RUNNABLE | appended to the tail |
| `sched_yield(t)` | RUNNING | RUNNABLE | as preemption, requested by the thread |
| block (`waitq_block`, `reply_link`) | RUNNING | BLOCKED_* | `current` passes to the next dispatch |
| stop (`thread_stop`, `exit`) | any but IDLE | INACTIVE or FAULTED | removed from what owned it; IPC detached (P64); continuation discarded (SC16) |

The ADR 0009 IPC primitives call these. No other code writes `t->state`
for a scheduling transition.

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

