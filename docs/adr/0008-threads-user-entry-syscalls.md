# ADR 0008: Threads, user entry, the syscall boundary and user-memory access

Status: accepted

Refines ADR 0003, step 4. This is the specification; it is implemented
(see "Implementation notes" at the end). Rule numbers (T1..T36) are referenced from
`docs/SECURITY.md`.

This step crosses from kernel infrastructure into running untrusted code.
Every rule below exists so that **no user-controlled value reaches a
privileged register, a kernel pointer or a kernel decision without being
copied once and validated**.

## Scope

In scope:
- Thread objects, their capability and lifecycle.
- The complete thread context and context switching.
- Entering user mode; returning from it via `syscall` or exceptions.
- The syscall ABI and dispatch.
- Copying to and from user memory.
- User fault handling.
- A minimal run queue.

Out of scope until later steps:
- IPC and fault *messages* (step 5). Until then a faulted thread is
  suspended and its fault record is readable through its Thread
  capability.
- Timer preemption and priorities (step 6). Until then threads run until
  they yield, block, fault or exit. See T36: this is a hard gate, not a
  convenience.
- SMP (ADR 0005). Rules are written so that per-CPU state is already
  per-CPU.

## Thread object

`CAP_THREAD` objects are retyped from Untyped (O3). Size is 8 KiB:

| Page | Contents |
|---|---|
| 0 | Thread control block: saved user registers, state, fault record, scheduling links, and **binding slots**: four capability slots inside the object |
| 1 | Extended register state (XSAVE area). Boot refuses to enable any XSAVE component whose area would not fit in 4 KiB. |

Like every object, it is zeroed at creation (O6), so a new thread starts
with every register zero.

### Binding slots

A thread refers to the resources it runs with only through capabilities
held in its own binding slots. Kernel references are capabilities, so
revocation reaches them (O10):

| Slot | Capability | Required for running |
|---|---|---|
| `TS_ASPACE` | AddressSpace | yes |
| `TS_CSPACE` | CNode (the thread's cspace root) | yes |
| `TS_IPCBUF` | Frame (IPC buffer; step 5) | no |
| `TS_FAULT` | Endpoint (fault handler; step 5) | no |

**T1. Binding is derivation.** `thread_bind(thread, slot, src)` derives a
copy of `src` into the binding slot, so it needs GRANT on `src`. A task
cannot give a thread a resource it could not hand to another task anyway.

**T2. Losing a required binding stops the thread.** When a capability in
`TS_ASPACE` or `TS_CSPACE` is deleted (directly, by revoking an ancestor,
or by destroying the object), the per-capability cleanup hook moves the
thread to `INACTIVE` *before* the deletion completes. If it is the current
thread, the kernel switches to the master PML4 and will not return to it.

### Thread capability rights

| Right | Allows |
|---|---|
| READ | `thread_read_regs`, `thread_read_fault`, `thread_state` |
| WRITE | `thread_write_regs`, `thread_bind`, `thread_unbind`, `thread_resume`, `thread_suspend`, `thread_set_priority` (step 6) |
| GRANT | copying the capability |

**T3. All control of a thread is through its capability.** One task can
read another's registers only with a Thread capability carrying READ, and
modify them, resume or suspend it only with WRITE. A thread does not
implicitly hold a capability to itself; it gets one only if one was
placed in its cspace. Holding a pointer to a thread, knowing its address,
or sharing its address space confers nothing.

### Lifecycle

| From | Event | To | Condition |
|---|---|---|---|
| (none) | `retype` | INACTIVE | Untyped with WRITE (O3) |
| INACTIVE | `thread_resume` | RUNNABLE | T4 holds |
| RUNNABLE | scheduled | RUNNING | T4 re-checked by the scheduler |
| RUNNING | `yield`, or another thread is scheduled | RUNNABLE | |
| RUNNING | user-mode fault, or invalid state on return (T17) | FAULTED | |
| RUNNING, RUNNABLE, FAULTED | `thread_suspend`, or loss of a required binding (T2) | INACTIVE | |
| FAULTED | `thread_resume` | RUNNABLE | T4 holds (the handler has repaired the state) |
| RUNNING | `exit` | INACTIVE | |
| any | last Thread capability deleted | destroyed | T5 |

**T4. Runnable implies runnable-safe.** `thread_resume` succeeds only if:
- `TS_ASPACE` holds an AddressSpace capability
- `TS_CSPACE` holds a CNode capability
- the saved entry state passes the user-entry checks (T11–T16)

The scheduler asserts the same conditions before every switch to a thread.
So the scheduler never runs a thread without a valid address space,
because no thread can be in its queue without one.

**T5. Destroying a thread is always safe.** When the last Thread
capability is deleted, the finaliser:
- removes the thread from every queue
- deletes its binding slots (which can stop nothing else, because they
  are copies)
- if it is the current thread, clears the per-CPU current pointer

Because the kernel stack is per-CPU (T8), nothing ever runs on memory
belonging to a destroyed thread.

## The complete thread context

**T6. A thread's context is exactly the following.** All of it is saved
on every entry to the kernel and restored on every return to the thread:

| Part | Contents | Notes |
|---|---|---|
| General registers | RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP, R8–R15 | |
| Instruction pointer | RIP | |
| Flags | RFLAGS, user-visible bits only (T13) | |
| Thread-local bases | FS base, GS base (user value) | Changed only through validated syscalls, or with `wrfsbase`/`wrgsbase` (only if CR4.FSGSBASE is enabled; it is **off** until the swapgs entry paths are audited for it) |
| Extended state | x87, MXCSR, SSE, AVX and every other component enabled in XCR0 | Saved with XSAVE(OPT), restored with XRSTOR, **eagerly on every switch**. Lazy FPU switching is forbidden: it leaked register contents across processes (CVE-2018-3665). |
| Address space | Implied by `TS_ASPACE` | Not a register value; CR3 is derived from the binding, never stored from user input |

Not part of the context, and never user-controlled:
- CS, SS: fixed user selectors, set by the kernel
- DS, ES, FS and GS selectors: null
- debug registers (DR7 = 0)
- CR0, CR4, EFER and every other control register

**T7. A new thread's extended state is the architectural initial
state.** Its XSAVE area header is zero, so XRSTOR loads init values. No
thread can ever observe another thread's registers, including vector
registers.

## Kernel stacks and entry

**T8. One kernel stack per CPU, with a guard page.** TSS.RSP0 points to it.
User state is saved into the current thread's TCB on entry. Kernel
operations either complete, or are restartable (like budgeted revocation).
The kernel never blocks on a stack.

**T9. Every entry from user mode does, before anything else:**
1. `swapgs`, only if the entry came from ring 3 (`CS & 3`). For NMI, #MC
   and #DF, which can interrupt kernel code at any point, decide from the
   GS-base MSR, not from CS.
2. `clac`. A user can set RFLAGS.AC; if it survived into the kernel through
   an interrupt or exception, it would silently disable SMAP. SYSCALL
   also clears AC through SFMASK (T19); `clac` covers every other entry.
3. `cld`, so string instructions in the kernel run forwards.
4. Switch to the per-CPU kernel stack. For SYSCALL the stub does this
   itself, since the hardware does not switch stacks.

**T10. Interrupts:** user code always runs with IF=1. The kernel runs with
interrupts disabled except at explicit preemption points.

**T32. One sanitised kernel-entry state, whatever the entry mechanism.**
Every transition into the kernel reaches common kernel C code only after
establishing all of the following. Every entry path must satisfy it:
SYSCALL, hardware interrupts, every exception vector, NMI, #MC, #DF, and
any mechanism added later.

| Property | How |
|---|---|
| RFLAGS.AC = 0 | `clac` in the stub (SFMASK as well for SYSCALL) |
| RFLAGS.DF = 0 | `cld` in the stub |
| Kernel stack valid | per-CPU kernel stack (T8), or for NMI, #MC and #DF a dedicated guarded IST stack; RSP is never taken from the interrupted context without switching |
| GS base is the kernel's | `swapgs` only if the entry came from ring 3, or decided from the GS-base MSR on IST entries that can interrupt the kernel anywhere |
| Kernel segments valid | CS and SS are set by hardware from the IDT/STAR; data segments are never used |
| User state captured | every general register saved before any C code runs; for entries from ring 3, copied to the current TCB (T6) |
| Address-space context known | the current thread and its bound AddressSpace are known from per-CPU data; CR3 is not trusted to identify the thread |
| Recovery state clean | no copy-routine recovery is armed (T35); an entry that finds one armed outside the copy routine's own range panics |

A single function, `entry_state_check()`, asserts these properties at the
start of the common C handler in test builds. Adding an entry path means
adding it to the test that drives every path through that assertion.

Interrupted kernel code is restored exactly: returning from an entry that
interrupted the kernel uses IRETQ with the saved RFLAGS. So an NMI that
arrives inside a copy routine's open-SMAP window clears AC for its own
execution, and AC is set again only for the interrupted copy.

**T33. Kernel stack usage is bounded, measured and budgeted.**
- Every per-CPU kernel stack and IST stack is 16 KiB with a guard page,
  and every kernel path must fit within half of it: **8 KiB worst case**.
- **No unbounded recursion in the kernel.** The only recursions allowed
  are listed here with their bound:
  - page-table subtree teardown: depth at most 4 (ADR 0007, V15)
- The recursive CNode finaliser (ADR 0006, known limitation) must become
  iterative before step 4 is complete: syscalls make CNode chains
  user-constructible. Its deletion must use the zombie-capability
  technique and preemptible, restartable progress, like revocation.
- Measurement: kernel stacks are painted with a known pattern at boot, and
  test builds report the high-water mark after the full suite. CI fails
  if it exceeds the budget. The compiler's `-fstack-usage` output is kept
  as a build artefact, so frame sizes can be reviewed per function.

## User entry state: validation

Every value that will be loaded into a privileged position on return to
user mode is checked. The checks run:
- in `thread_write_regs`, which changes nothing if any check fails (`T_EINVAL`)
- in `thread_resume`
- on every return to user mode

**T11. RIP** must be canonical and in the user half (`< USER_TOP`).

**T12. RSP** must be canonical and in the user half. It need not be
mapped; a bad stack just faults the thread in user mode.

**T13. RFLAGS:** user-settable bits are CF, PF, AF, ZF, SF, DF, OF, AC
and ID. On return the kernel forces:
- IF = 1
- IOPL = 0
- TF = 0 (single-stepping comes later, behind a debug capability)
- NT = 0, RF = 0, VM = 0, VIF = 0, VIP = 0
- reserved bit 1 = 1

`thread_write_regs` rejects any value with a bit outside the user-settable
set (it doesn't silently fix it), so a supervisor learns that it asked for
something impossible.

**T14. CS and SS are not part of the writable register set at all.** The
kernel always returns with the user code and data selectors at RPL 3.
There is no field in which to supply a kernel selector.

**T15. FS base and GS base** must be canonical and in the user half.
Loading a non-canonical base would fault inside the kernel.

**T16. Extended state cannot be written by another thread in this step.**
Loading a malformed XSAVE image with XRSTOR faults in the kernel. When
remote extended-state writes are added (for debuggers), the image must
first be validated: header reserved bits zero, XSTATE_BV ⊆ XCR0,
compaction bits consistent.

**T17. If a check fails on the return path, the thread is faulted, not the
kernel.** This should be impossible, given the checks at write and resume
time, but the return path does not rely on that.

### Return path: SYSRET versus IRET

**T18. SYSRET only when it is provably safe; IRET otherwise.**

On Intel CPUs, SYSRET with a non-canonical RCX raises #GP *in ring 0*,
while RSP is already the user's stack. That is the class of
CVE-2012-0217 and CVE-2014-4699. It happens naturally when a 2-byte
`syscall` instruction sits in the last two bytes of user space: the
saved return address is `USER_TOP`, which is non-canonical.

So the kernel never executes SYSRET unless **the complete return state is
proven safe for SYSRET's specific semantics**. SYSRET loads RIP from RCX,
RFLAGS from R11 and CS/SS from STAR, and does not switch stacks. All of
these must hold:

- The thread entered with SYSCALL, and nothing has rewritten its RIP,
  RSP or RFLAGS since. Any external `thread_write_regs` forces IRETQ.
- RCX is canonical and `< USER_TOP`, which is checked explicitly.
- R11 is the sanitised user RFLAGS (T13): IF=1, IOPL=0, TF=0, NT=0, RF=0,
  VM=0. It is recomputed from the TCB, never taken from the live
  register.
- STAR selects the fixed user CS and SS, which is checked once at boot.
- In the window after the user RSP and user GS base are restored and
  before SYSRET retires, only NMI and #MC can arrive (IF=0). Both use IST
  stacks and decide GS from the MSR (T32), so neither runs on the user's
  RSP or with the user's GS.

If any condition is not met, the return uses IRETQ with a fully validated
frame.

Every other return goes through IRETQ with a full, validated frame. That
includes returns from exceptions or interrupts, after `thread_write_regs`,
after a thread's first resume, and the boundary case above.

## Syscall entry and ABI

**T19. MSR configuration:**
- `STAR` = kernel and user selector bases (GDT layout already fixed in
  `gdt.h`)
- `LSTAR` = the entry stub
- `SFMASK` clears IF, TF, DF, AC and NT on entry
- EFER.SCE is set

**T20. ABI:**
- RAX = syscall number
- arguments in RDI, RSI, RDX, R10, R8, R9 (R10 instead of RCX, which
  SYSCALL clobbers)
- return value in RAX: 0 or a value on success, a negative `T_E*` on error
- RCX and R11 are clobbered; every other register is preserved

**T21. Every argument is read exactly once,** from the TCB copy saved at
entry. The handler receives a kernel-owned argument array, never the live
registers.

**T22. The syscall number is bounds-checked and then masked** with a
branchless index mask before indexing the table, so a mispredicted bounds
check cannot speculatively index past it (Spectre v1). Unknown numbers
return `T_EINVAL`; they do not fault the thread.

**T23. Capabilities remain the only authority.**
- Every syscall except `yield` and `exit` takes at least one capability
  slot, resolved in the calling thread's bound cspace through the
  existing `cap_lookup` / `cnode_view` paths, with the same rights checks
  as the in-kernel API.
- The syscall layer adds no permission logic of its own: it is
  marshalling only.
- A syscall can never name a capability by kernel address, and never
  resolves a slot in any cspace other than the caller's.

**T24. The initial syscall set is small.**

| Syscall | Authority |
|---|---|
| `cap_identify`, `cap_derive`, `cap_delete`, `cap_revoke` | the capabilities named (*ADR 0010 SC22:* `cap_revoke` takes no budget and returns when complete) |
| `retype` | Untyped with WRITE (O3) |
| `vspace_install`, `vspace_map`, `vspace_unmap`, `vspace_protect` | per ADR 0007 |
| `thread_bind`, `thread_unbind`, `thread_read_regs`, `thread_write_regs`, `thread_resume`, `thread_suspend`, `thread_read_fault` | Thread capability rights (T3) |
| `debug_write` | a `CAP_DEBUG_CONSOLE` capability given only to the initial task. Even the temporary console needs authority. |
| `yield`, `exit` | none: they affect only the caller |

## User-memory access

**T25. User pointers are a distinct type,** `uaddr_t`: a struct wrapping an
integer. It cannot be dereferenced, cast implicitly, or passed where a
kernel pointer is expected. `kernel_struct = *(user_pointer)` does not
compile. The only operations on a `uaddr_t` are the copy routines.

**T34. Address kinds are distinct types throughout the kernel.**

| Kind | Type | Converts by |
|---|---|---|
| Kernel virtual address | a C pointer | (dereferenceable) |
| User virtual address | `uaddr_t` (struct) | copy routines only |
| Physical address | `paddr_t` (struct) | `phys_to_virt()` / `virt_to_phys()`, explicit constructors |
| Integer that is an address but not yet one of the above | `uint64_t` | must be wrapped at the boundary where it is validated |

Converting the existing `paddr_t` (today an integer typedef) to a struct
is the first step of the implementation. Mixing up physical, user and
kernel addresses then becomes a compile error, not a code-review
finding.

**T26. `copy_from_user(kdst, usrc, len)` and `copy_to_user(udst, ksrc, len)`:**
1. Compute `end = usrc + len` with overflow checking. Require
   `end <= USER_TOP` and `len <= COPY_MAX` (4 KiB in this step).
2. Open SMAP with `stac`, copy, then close it with `clac`.
3. A page fault inside the copy is caught by an exception fixup table
   that lists the copy routine's address range, and returns `T_EFAULT`.
   A fault anywhere else in kernel mode remains a kernel bug and panics.

These are the only places in the kernel where `stac` appears; a CI check
greps for it.

**T35. Fault recovery in the copy routines is exact.**

```text
copy_from_user()
   ├── range checked (T26 step 1)
   ├── stac
   ├── copy loop: the only instructions listed in the fixup table
   ├── clac
   └── return 0
fault in the copy loop
   └── #PF handler: faulting RIP in the fixup table AND fault address < USER_TOP
          → resume at the routine's landing pad: clac, return T_EFAULT
```

- Recovery is armed by the faulting **instruction address** being in the
  fixup table, not by a flag that could be left set. There is no state
  to forget to clear.
- The landing pad is the only place execution can go after a recovered
  fault. It always executes `clac` before returning, so SMAP is restored
  on success, on fault, and on every path in between.
- A fault in the copy loop at a kernel address (which the range check
  makes impossible) is not recovered: it panics. So a range-check bug
  cannot be silently turned into "return an error".
- Copy routines are leaf assembly: they call nothing, so copies cannot
  nest, and nothing else ever runs with SMAP open. An interrupting NMI or
  #MC clears AC on entry and IRETQ restores it (T32).
- A partially accessible buffer copies up to the fault and then reports
  `T_EFAULT`. The caller must treat the destination as unusable: callers
  never use a buffer from a failed copy, and `copy_from_user` zeroes the
  whole destination on failure, so no partial or stale bytes survive.

**T27. Single fetch.** Every syscall that takes user memory copies it into
a kernel buffer once, validates the copy, and then uses only the copy.
Patterns like

```c
if (u->size < MAX) copy(u->buf, u->size);   /* forbidden */
```

cannot be written, because `u` is a `uaddr_t` (T25).

**T28. No kernel data leaks out.**
- `copy_to_user` copies only fully initialised kernel buffers. Structures
  crossing the boundary have explicit padding fields that are set to zero,
  plus a `_Static_assert` on their size, as with `struct cap_info`.
- On every return to user mode, every general register is reloaded from
  the TCB, so no kernel value survives in a register.

## Faults

**T29. A fault in user mode never panics the kernel** (U4). Exceptions with
`CS & 3 == 3`:
- #DE, #DB, #BP, #UD, #NM, #GP, #SS, #PF, #AC, #MF, #XM and #CP
- an invalid user state detected on return (T17)

each produce a **fault record** (vector, error code, CR2 for #PF, faulting
RIP), stored in the TCB. The thread moves to `FAULTED`. The record is
readable with READ on the Thread capability.

In step 5 the record becomes a message to the `TS_FAULT` endpoint, whose
handler can repair the fault and resume the thread, or destroy it.

**T30. Kernel-mode faults still panic,** except faults inside the copy
routines (T26), which return `T_EFAULT`. #DF, #MC and NMI are always kernel
matters.

**T31. Syscall errors are return values, not faults.** These return an
error to the caller and leave the thread running:
- unknown syscall number: `T_EINVAL`
- bad capability slot: `T_ERANGE` or `T_EBADCAP`
- insufficient rights: `T_EPERM`
- unmapped or kernel user pointer: `T_EFAULT`

**T36. No untrusted program runs before timer preemption exists.**
*ADR 0010 (SC9-SC14) specifies how T36 is met: it is satisfied once
ADR 0010's T36 tests pass.*
Until step 6, the only user programs are the kernel's own test programs.
The first program not written as part of the kernel test suite runs only
once:
- timer interrupts preempt user threads
- the scheduler guarantees that a thread which never yields cannot keep
  the CPU from other runnable threads

## Invariants introduced

| ID | Invariant | Rules |
|---|---|---|
| T-A | Thread state is read or changed only with a Thread capability carrying the right | T3 |
| T-B | A thread can be runnable only with a bound address space, a bound cspace, and valid entry state | T2, T4 |
| T-C | No user-controlled value reaches RIP, RSP, RFLAGS, CS, SS, FS or GS base, or XRSTOR without validation | T11–T16 |
| T-D | The complete context, including all extended state, is saved and restored on every switch; no thread observes another's registers | T6, T7, T28 |
| T-E | Every kernel entry from user mode clears AC and swaps GS correctly; SMAP is never silently disabled | T9, T19 |
| T-F | SYSRET never runs with a non-canonical return address | T18 |
| T-G | The kernel touches user memory only through the copy routines, reading each value once | T25–T27 (completes K3) |
| T-H | Syscalls are authorised only by capabilities | T23 |
| T-I | A user fault never panics the kernel | T29 (completes U4) |
| T-J | Every kernel entry establishes the same sanitised state (T32) before common code runs | T9, T32 |
| T-K | Worst-case kernel stack use is within budget; no unbounded kernel recursion | T33 |
| T-L | A fault during a user-memory copy always returns `T_EFAULT` through the landing pad with SMAP restored; no other kernel fault is recovered | T35 |
| T-M | Physical, user and kernel addresses cannot be confused without a compile error | T25, T34 |

## Tests required before step 4 is done

User test programs are small position-independent routines in the test
kernel, copied into Frames mapped RX at user addresses.

1. **Hostile register state** via `thread_write_regs`: each of the
   following is rejected with `T_EINVAL` and leaves the thread unchanged:
   - RIP or RSP at a kernel address, `USER_TOP`, or non-canonical
   - RFLAGS with IOPL, TF, NT, VM, VIF or VIP set
   - FS or GS base at a kernel address or non-canonical

   There is no way to express a kernel CS or SS.
2. **Privilege:** a user program reads its own CS and SS and reports them
   through a syscall: RPL 3 and the user selectors.
3. **The SYSRET boundary:** a `syscall` instruction placed in the last two
   bytes below `USER_TOP`. The kernel survives and returns by IRET (or
   faults the thread), never by SYSRET.
4. **AC on entry:** a user program sets AC, then performs a syscall, and
   separately triggers #UD. Kernel-side hooks record RFLAGS at entry: AC
   is clear in both paths.
5. **Every fault vector from ring 3:** #DE, #BP, #UD, #GP, #SS, #PF (read,
   write, execute), #AC, #XM. The thread ends `FAULTED` with the right
   record, the kernel continues, and other threads still run.
6. **Malformed syscalls:**
   - unknown number
   - out-of-range slot
   - wrong capability type
   - insufficient rights
   - pointers that are unmapped, kernel-half, non-canonical, or that wrap
     around the address space
   - length over `COPY_MAX`

   Each returns the specified error without faulting the thread.
7. **Single fetch:** a second thread rewrites a user structure while the
   kernel processes a syscall that reads it. Without preemption this is
   modelled by a kernel test hook that mutates the user buffer between the
   copy and the use. The kernel's behaviour depends only on its copy.
8. **Register isolation:**
   - A new thread's first instruction finds every general and vector
     register zero.
   - Thread A puts a pattern in XMM, YMM and x87 state, then yields;
     thread B sees initial state.
   - When A resumes, its pattern is intact.
   - After any syscall, the registers the ABI preserves are unchanged, and
     no register carries a kernel address.
9. **Lifecycle:**
   - resume without bindings: `T_EPERM`
   - revoke the bound AddressSpace while the thread is runnable, and while
     it is the current thread: the thread becomes INACTIVE, the kernel is
     on the master PML4, and the system continues
   - delete the running thread's last capability from inside a syscall it
     made: no use-after-free
   - a thread without READ/WRITE on another's Thread capability can neither
     inspect nor change it
10. **Round trip:** user → syscall → kernel → user, in thousands of
    iterations with randomised argument registers. Preserved registers stay
    preserved, and RAX is always a valid result.
11. **Every entry path through `entry_state_check()` (T32):** syscall,
    every ring-3 exception vector, a timer or self-IPI interrupt from ring
    3 and from ring 0, a self-NMI (through the local APIC) while in user
    mode, while in ordinary kernel code, and inside a copy routine's open-
    SMAP window. Each entry observes AC=0, DF=0, the correct GS base and a
    valid stack.
12. **Stacks (T33):**
    - stack high-water mark within budget after the whole suite
    - guard-page hit on the per-CPU kernel stack ends in a clean #DF panic
      on the IST stack, never memory corruption
    - exception during exception handling, and NMI during kernel execution,
      both handled on their own stacks
    - a user-built chain of 1000 nested CNodes is deleted without
      exceeding the stack budget
13. **Copy routines (T35):**
    - fault on the first byte, on the last byte, and on a page-boundary
      crossing
    - a partially accessible buffer: destination fully zeroed, `T_EFAULT`
    - length overflow and wraparound
    - zero-length copy
    - after every one of these, AC is clear and the next ordinary kernel
      access to user memory faults (SMAP still active)

    Mutation tests specifically for: SMAP not restored on the fault path,
    recovery accepted for a non-table RIP, recovery accepted for a
    kernel-half fault address, a missing range check, no zeroing on
    failure, and an off-by-one at the end of the range.

Then, as before:
- mutation testing of the entry and exit stubs, the validators, the copy
  routines and the fault paths
- a security review against this document
- documentation updates

## Implementation order

1. FPU and XSAVE enablement at boot; per-CPU kernel stack and per-CPU data
   via GS base; SYSCALL MSRs.
2. Thread object, binding slots, capability operations (no execution
   yet).
3. Entry and exit stubs (syscall, exceptions from ring 3), SYSRET/IRET
   selection, fault records.
4. Copy routines with the fixup table; the `uaddr_t` type.
5. Syscall table and the initial syscalls.
6. Minimal run queue, `yield` and `exit`, then the test programs and the
   tests above.

## Implementation notes

Implemented in full. Deviations and findings during implementation:

- **SMEP and SMAP are mandatory.** `clac`/`stac` raise #UD on CPUs without
  SMAP and kernel text is read-only, so the kernel refuses to boot without
  them rather than patching code.
- **CR0.AM is set**, so user misalignment with AC set raises #AC. It never
  applies in ring 0, where AC is always clear.
- **Kernel-held frames are counted.** Mapping the xAPIC MMIO page takes
  page-table frames from the kernel reserve; the ownership invariant (O1)
  now checks three-way conservation.
- **QEMU's software emulator** cannot produce several hardware behaviours
  the tests target: #GP/#SS for non-canonical user accesses (it raises
  #PF), #XM and #AC, and an NMI arriving in user mode. QEMU also emulates
  SYSRET with AMD semantics. The tests accept the emulator's outcome for
  these and print a note; they must be confirmed on real hardware or KVM.
- **Equivalent mutants:** two deliberate bugs in thread code (dropping
  bind's own GRANT check; not dequeuing a destroyed thread) are not
  caught, because the same property is enforced on another path
  (derivation checks GRANT; destroying a runnable thread deletes its
  address-space binding, which already dequeues it).
