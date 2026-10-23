# ADR 0008: Threads, user entry, the syscall boundary and user-memory access

Status: proposed

Refines ADR 0003, step 4. This is the specification; nothing in step 4 is
implemented until it is agreed. Rule numbers (T1..T31) are referenced from
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
  they yield, block, fault or exit. Acceptable only because no untrusted
  code runs before step 6.
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

So the kernel returns with SYSRET only if:
- the thread entered with SYSCALL
- nothing has rewritten its RIP, RSP or RFLAGS since
- RCX (the return RIP) is `< USER_TOP`, which is checked explicitly

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
| `cap_identify`, `cap_derive`, `cap_delete`, `cap_revoke` | the capabilities named |
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

**T26. `copy_from_user(kdst, usrc, len)` and `copy_to_user(udst, ksrc, len)`:**
1. Compute `end = usrc + len` with overflow checking. Require
   `end <= USER_TOP` and `len <= COPY_MAX` (4 KiB in this step).
2. Open SMAP with `stac`, copy, then close it with `clac`.
3. A page fault inside the copy is caught by an exception fixup table
   that lists the copy routine's address range, and returns `T_EFAULT`.
   A fault anywhere else in kernel mode remains a kernel bug and panics.

These are the only places in the kernel where `stac` appears; a CI check
greps for it.

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
