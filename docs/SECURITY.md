# Security model

## Principle

Tarka does not claim to be bug-free. It is designed to **minimise the
impact of vulnerabilities, detect failures early, and distribute verified
fixes quickly**.

The hardening mechanisms below are not the security model on their own.
They are layers of damage limitation, ordered so that when one layer fails
the next still stands:

```text
capability checks      (a task can only name what it was given)
  ↓ fails
address-space isolation (it still cannot touch other tasks' memory)
  ↓ fails
SMEP / SMAP            (the kernel still will not run or read its data)
  ↓ fails
W^X / NX               (injected bytes still cannot execute)
  ↓ fails
stack canaries, guards (overwrites are still detected before use)
```

## Assets

| Asset | Why it matters |
|---|---|
| Kernel memory and code | Whoever controls it controls everything |
| Capability spaces | They define all authority in the system |
| User memory | Each task's data must stay private to it |
| Device access (registers, I/O ports, IRQs, DMA) | Hardware access bypasses software isolation |
| Cryptographic keys and random generator state | Predictable keys or randomness break every protocol built on them |
| Filesystem data, network credentials | User data and secrets |
| Boot chain integrity | A modified kernel nullifies every other protection |
| Update signing keys | Whoever holds them can push code to every machine |

## Trust boundaries

```text
firmware            trusted, unverified (out of scope for now)
  │  verified boot (planned)
bootloader          trusted, pinned by commit
  │  loads and maps kernel W^X
kernel              trusted computing base: kept small
  │  syscall ABI + capability checks  ◄── the main boundary
user-space servers  untrusted by the kernel and by each other
  │  IPC with explicitly granted capabilities
applications        untrusted
```

Devices are a separate boundary: a device is untrusted and is confined by
the IOMMU to the buffers its driver was granted.

## Attackers considered

| Attacker | Starting position | Must not be able to |
|---|---|---|
| Malicious application | Code execution in one task | Gain capabilities, read others' memory, crash the kernel |
| Compromised server or driver | Full control of one server | Exceed the capabilities that server holds |
| Malicious device or DMA | Bus mastering | Access memory outside its IOMMU domain |
| Malicious file or network peer | Crafted input to a parser | Escape the parsing server's sandbox |
| Malicious boot media | Replaced kernel or bootloader | Boot unsigned code (once verified boot exists) |
| Malicious update server | Serves arbitrary update files | Install unsigned, older or frozen updates |

Out of scope for now: physical RAM attacks, malicious firmware, side
channels beyond the standard Spectre/Meltdown mitigations.

## Security invariants

Each invariant is a statement that must always hold. The O-series are
defined precisely in [ADR 0006](adr/0006-untyped-retype.md), the V-rules
behind U2, U3 and U5 in [ADR 0007](adr/0007-address-spaces.md). Each one is proven by
a self-test that runs in CI (`make test`), or is marked as planned until
the code it depends on exists. Adding a subsystem means adding its
invariants and tests here.

| ID | Invariant | Proof | Status |
|---|---|---|---|
| K1 | Kernel executable memory is never writable | self-test `w^x` (page-table walk); write injection faults | ✅ tested |
| K2 | Kernel data and read-only data are never executable | self-test `w^x` | ✅ tested |
| K3 | The kernel cannot execute or access user memory outside explicit copy routines | SMEP/SMAP mandatory at boot; `scripts/check-stac.sh` (stac only in the copy routine); self-test `uaccess` | ✅ tested |
| K4 | A kernel stack overflow faults; it never corrupts adjacent memory | self-test `stack_guards`; recursion injection gives a clean #DF | ✅ tested |
| K5 | A stack buffer overflow is detected before the function returns | self-test `cpu_hardening` (canary is random); smash injection panics | ✅ tested |
| M1 | Memory given to a new owner contains no data from a previous owner | self-test `pmm` (zero on alloc and free) | ✅ tested |
| M2 | The allocator never releases memory it does not own, and detects double frees | self-test `pmm` | ✅ tested |
| C1 | Rights can never increase through derivation | self-test `capabilities` | ✅ tested |
| C2 | Every capability use is checked for bounds, type and rights | self-tests `capabilities`, `syscalls` (from ring 3, incl. 64-bit slot aliasing) | ✅ tested |
| C3 | A badge cannot change once set | self-test `capabilities` | ✅ tested |
| C4 | Capability data given to user space contains no kernel pointers and no uninitialised bytes | `struct cap_info` + `_Static_assert`; self-test `capabilities` (`cap_identify`) | ✅ tested |
| C5 | Revoking a capability removes every capability derived from it, in every cspace, including after intermediate deletions | self-test `cap_revoke` | ✅ tested |
| C6 | Deleting the last capability to an object finalises the object exactly once, and never earlier | self-test `cap_revoke` (finaliser hook) | ⚠️ hook tested; object types arrive with ADR 0003 steps 2–5 |
| C7 | No derivation-tree link ever points to a deleted capability or a destroyed cspace | self-test `cap_revoke` (`cdt_consistent`, `links_within`) | ✅ tested |
| U1 | A task cannot obtain a capability it was not explicitly given | | planned (ADR 0003) |
| U2 | A task cannot map kernel memory or another task's memory without a frame capability. Structural: user leaves are only ever Frames, user intermediate entries only PageTables, the kernel half is supervisor-only at the top level (ADR 0007, V1–V4, V9) | self-tests `vspace_deputy`, `vspace_*` audits | ✅ tested |
| U3 | No user mapping is both writable and executable, and no physical frame is writable through one mapping while executable through another; in-place changes only remove rights (ADR 0007, V5–V8) | self-tests `vspace_perms`, `vspace_alias` | ✅ tested |
| U5 | Removing a mapping, table or address space leaves no stale entry, shadow pointer or record, and an uninstalled table carries no mappings (ADR 0007, V11, V12, V15) | self-test `vspace_teardown` + structural audit | ✅ tested |
| U4 | A user-space fault never panics the kernel | self-test `user_faults` (13 fault kinds, healthy thread keeps running) | ✅ tested |
| T-A | Thread state is read or changed only with a Thread capability carrying the right | self-test `thread_lifecycle` (rights matrix from a second task) | ✅ tested |
| T-B | A thread can be runnable only with a bound address space, a bound cspace, and valid entry state | self-tests `thread_lifecycle`, `syscall_lifecycle` (self-revoke, self-destroy) | ✅ tested |
| T-C | No user-controlled value reaches RIP, RSP, RFLAGS, CS, SS, FS or GS base, or XRSTOR without validation | self-tests `thread_regs` (23 hostile values), `user_entry_state`, `user_sysret_edge` | ✅ tested |
| T-D | The complete thread context, including all extended (FPU/vector) state, is saved and restored on every switch; no thread observes another's registers | self-test `user_basics` (fresh state, XMM/YMM/x87 isolation, 2000 randomised round trips) | ✅ tested |
| T-E | Every kernel entry from user mode clears AC and swaps GS correctly; SMAP is never silently disabled | self-tests `user_entry_state`, `entry_paths` | ✅ tested |
| T-F | SYSRET never runs with a non-canonical return address | self-test `user_sysret_edge` (syscall in the last two bytes of user space) | ✅ tested (QEMU emulates SYSRET with AMD semantics; the Intel ring-0 #GP path is prevented by construction, not observed) |
| T-G | The kernel touches user memory only through the copy routines, reading each value once | `uaddr_t`; self-tests `uaccess`, `syscalls` (buffer rewritten after the copy) | ✅ tested |
| T-H | Syscalls are authorised only by capabilities; the syscall layer adds no permission logic | self-test `syscalls` (28 cases from ring 3) | ✅ tested |
| T-J | Every kernel entry (syscall, interrupt, exception, NMI, #MC, #DF) establishes the same sanitised state before common code runs | `entry_state_check()` on every user entry; self-tests `user_entry_state`, `entry_paths` | ⚠️ all paths tested except NMI arriving in user mode (not producible under QEMU TCG) |
| T-K | Worst-case kernel stack use is within an 8 KiB budget, measured in CI; no unbounded kernel recursion | stack painting + budget check after the suite; self-test `cnode_chain` (1000 levels) | ✅ tested |
| T-L | A fault during a user-memory copy always returns `T_EFAULT` through the landing pad with SMAP restored; no other kernel fault is recovered | self-tests `uaccess`, `entry_paths` (NMI inside the open-SMAP window) | ✅ tested |
| T-M | Physical, user and kernel addresses cannot be confused without a compile error | `scripts/check-address-types.sh` (10 misuses must not compile) | ✅ tested |
| O1 | Every physical frame has exactly one owner: the kernel allocator or one Untyped; total memory is conserved | self-test `untyped_ownership` | ✅ tested |
| O2 | Memory authority is never duplicated: Untypeds cannot be copied | self-test `untyped_rules` | ✅ tested |
| O3 | Retype never creates authority from nothing: it needs an Untyped with WRITE and free space in it | self-test `untyped_rules`, `untyped_exhaustion` | ✅ tested |
| O4 | A failed retype changes nothing: no capability, no watermark movement | self-test `untyped_rules` | ✅ tested |
| O5 | Objects created from one Untyped never overlap, are aligned to their size, and lie inside it | self-test `untyped_overlap` (randomised, nested) | ✅ tested |
| O6 | Every object is zeroed when created | self-test `untyped_exhaustion` | ✅ tested |
| O7 | Memory is reusable exactly when its Untyped has no children | self-test `untyped_exhaustion` | ✅ tested |
| O8 | An Untyped with children cannot be deleted | self-test `untyped_exhaustion` | ✅ tested |
| O9 | Revoking an Untyped destroys everything created from it, however nested, in any cspace | self-test `untyped_revoke` | ✅ tested |
| O10 | Only capabilities keep objects alive; finalisers remove every internal reference before memory can be reused | self-test `untyped_revoke` (CNode contents) | ⚠️ CNode tested; applies to each new object type |
| O11 | Revocation stays correct when finalisers delete other capabilities | self-test `untyped_revoke` | ✅ tested |
| I-A1 | IPC is atomic: it commits completely or produces no observable change to any cspace, queue, thread state, IPC buffer, reply link or notification | exact before/after snapshots in `ep_*`, `ep_xfer_*`, `reply_*`, `call_offer`, `fault_contract`, `notif_*` | ✅ tested |
| I-T1 | Capabilities move by IPC only with GRANT on the endpoint and on each capability (for replies, the GRANT recorded at call time), into validated empty slots the receiver offered; each is a CDT child of the sender's | self-tests `ep_xfer_*`, `call_basic`, `reply_refuse` (call_grant in both arrival orders) | ✅ tested |
| I-E2 | A queued sender leaves BLOCKED_SEND with an error at most once: queue-discard work cannot charge the same sender twice | self-test `ep_xfer_drain` | ✅ tested |
| I-C1 | Transfer conservation: a successful transfer of N capabilities adds exactly N records, each a CDT child of its source, and changes nothing else; a failed transfer changes nothing | byte-exact world snapshots of every cspace and binding slot (`world_transferred`, `world_same`) in every `ep_xfer_*` test | ✅ tested |
| I-SC6 | The CDT is a well-nested bracket list that represents exactly the derivation forest: deletion re-parents children to the deleted record's parent, and revocation removes one record per O(1) step (ADR 0010) | reference-forest differential tests `cdt_forest`, `cdt_random`; `cdt_untyped_descent`; `cdt_revoke_bound`; `cdt_consistent()` audits | ✅ tested |
| I-SC5 | Every object's live-record count, endpoint and notification READ/WRITE counts, and frame writable/executable alias counts equal the live records and aliases, after every step; failed operations change no count; no metadata survives its object's last record (ADR 0010, SC17-SC19) | independent oracle `meta_audit()` (records found by scanning storage, never via the CDT; mappings read from hardware entries) after every test and in every IPC audit; self-tests `meta_counts`, `meta_frames`, `meta_overflow`; 26 mutants caught, the 18 counter mutants also with kernel assertions compiled out | ✅ tested |
| I-SC8 | "Last capability to an object" is decided by the live-record count alone, never by CDT position (ADR 0010, SC19a) | `scripts/check-cdt.sh` (gate SC31.2) in `make test`; positional-finality mutant caught | ✅ tested |
| F1 | No new capability record enters a frozen derivation subtree: derive, transfer, binding and retype refuse a frozen object or carving chain with `T_EBUSY`; records are inserted only by the derivation and retype commits; a revocation freezes its object for exactly as long as it runs (ADR 0010, SC22a, SC22b, revision 3e E1) | self-tests `meta_freeze`, `revoke_chunks`, `revoke_cancel`, `revoke_syscall` (derivers refused throughout a user revocation); `scripts/check-cdt.sh` (gate SC31.5); oracle: marked root, frozen object and revocation continuation correspond one to one; freeze mutants caught | ✅ tested |
| I-SC7 | Dying objects grant nothing, are owned by exactly one reaper entry and one Untyped, are counted by that Untyped from the step their last record goes until their teardown completes, and their memory is never reused before then; reaper work is at most `REAP_CHUNK` units per boundary and never waits (requeue, D2) (ADR 0010 SC23, SC24, R0-R9, revision 3d) | self-tests `reaper_dying`, `reaper_aba` (the pin alone keeps a new object off a dying object's address; verified with the pin removed and the oracle off), `reaper_requeue` (D2's proof checked on a trace of every unit), `cnode_chain`; oracle audits of the reaper list and every Untyped's dying count; 24 mutants caught | ✅ tested |
| R6 | Stopping or destroying the thread that caused an object's death has no effect on its teardown (ADR 0010 R6) | self-test `reaper_r6`: the deleter suspends or destroys itself in its next syscall, with no reaper unit done in between (asserted), and the teardown is exact | ✅ tested (construction-derived until stage 4) |
| I-CT | A continuation holds no kernel pointer; every chunk resolves its target again by slot and identity before touching it; only the final chunk writes the result; every stop releases what it holds and leaves `T_EINTR` (ADR 0010 SC16, revision 3e E0, E4) | IPC audit rule 12; self-tests `revoke_syscall`, `revoke_no_early_result`, `drain_identity`, `drain_chunks`; mutants (identity, mark, cursor, early result, no abandonment, no `T_EINTR`) caught | ✅ tested |
| I-E2' | A `reply_recv`'s reply is delayed only by the senders queued when it began; later senders delay only the server (ADR 0009 P63 as amended, ADR 0010 revision 3e E2) | self-test `drain_reply_bound` (re-sending adversaries); mutants (snapshot ignored, cursor not decremented) caught | ✅ tested |
| I-ID | An object's identity is its type, address and serial; serials are never reused, and creation fails with `T_ENOMEM` rather than wrap (ADR 0010 SC21, revision 3d D0, D3) | self-tests `reaper_aba` (stale identities match nothing, before and after the address is reused), `serial_exhaustion`; oracle: every serial issued, unique, newer than its carver's | ✅ tested |
| V15' | A populated table that is not installed is inert and cannot be installed; detaching never walks below the detached table (ADR 0007 as amended by ADR 0010 revision 3d D1) | self-test `vspace_teardown`; oracle compares every table's filled count with its hardware entries; mutants (no install check, uninstall destroying, filled count not kept, AddressSpace death not detaching) caught | ✅ tested |
| I-SC1 | Every thread is owned by exactly the place its state names: RUNNABLE in its home run queue exactly once, RUNNING as its CPU's `current`, BLOCKED in one wait queue or one reply link, INACTIVE and FAULTED by nothing; queues are no longer than the live threads (ADR 0010 SC1, SC29, G8) | `ipc_audit()`, which derives each thread's owner by walking every run queue and wait queue link by link and compares with the state and the stored lengths; it is itself tested against ten corruptions (`sched_audit_detects`) | ✅ tested |
| I-SC2 | Each state change is one of the edges of SC6, made by its one primitive in `core/sched.c`, which commits the state and run-queue membership together; any other pair is refused with `T_EINVAL` and nothing changed (ADR 0010 G1, G2, G11) | gate `scripts/check-sched.sh` in `make test` (no write of the state, run-queue links, `home_cpu` or `current` outside `sched.c`; shown to catch 13 write forms); self-test `sched_edges`: every primitive against a thread in every state, including `current` while blocked or stopped, byte-for-byte unchanged on refusal | ✅ tested |
| I-SC3 | `current` is never NULL after boot; the idle thread is clean, unqueued, unregistered and not addressable; a thread that destroys itself hands `current` to the idle thread before any of it is torn down (ADR 0010 SC2, SC5, G3) | assertion at every scheduling boundary; self-tests `sched_current` (self-suspension, self-destruction with a teardown-ordering probe), `sched_audit_detects` | ✅ tested |
| I-SC4 | The idle loop halts with `sti; hlt` and is woken by an interrupt; the test driver regains control only at the pre-HLT boundary with nothing armed, never in place of an armed halt (ADR 0010 G4) | self-test `sched_halt_wake`: a self-IPI and a LAPIC one-shot, each taken at the instruction after the idle thread's HLT | ✅ tested |
| I-SC5' | The scheduler's work is O(1) per primitive; the idle loop does one reaper chunk per pass; a thread with a continuation gets no priority (ADR 0010 G5, G6, R5) | self-tests `sched_idle_reap` (32 units in full `REAP_CHUNK` passes), `sched_fair_cont` (strict round robin while a revocation runs) | ✅ tested |
| I-R1 | Reply authority is single-use, non-transferable, and answers only its own caller | self-tests `call_basic`, `reply_refuse`, `reply_gone`, `reply_recv`, `reply_recv_edges`, `ipc_reply_links` | ✅ tested |
| I-S1 | Thread states and IPC queues are always consistent; a blocked thread is never runnable | `ipc_audit()` after every IPC test step (and itself tested against corruption); self-tests `ipc_*`, `notif_*` | ✅ tested |
| I-L1 | No thread stays blocked on an operation that can no longer happen; no queue or link refers to a dead object; endpoint/notification capability counts equal the live rights-bearing records | self-tests `ipc_states`, `ipc_reply_links`, `notif_liveness`, `notif_destroy`, `ep_liveness`, `ep_destroy` | ✅ tested |
| I-N1 | A set notification bit is delivered to exactly one waiter/poller or stays pending until consumed; repeated signals coalesce; only wait/poll consume bits; a signaller sets only its badge's bits; badges are inherited exactly and only unbadged capabilities mint | self-tests `notif_semantics`, `notif_badges`, `notif_waiters` | ✅ tested |
| I-F1 | Fault messages carry only the faulting thread's own state; a handler controls a thread only through its Thread capability; fault delivery never recurses | self-tests `fault_deliver`, `fault_authority`, `fault_nohandler`, `fault_handler_gone`, `fault_self`, `fault_nested`, `fault_kernel_paths`, `fault_contract` | ✅ tested (NMI/#DF/#MC from ring 3: by construction, not testable under QEMU) |
| I-B1 | IPC work, message size and queue depth are bounded; IPC never allocates and performs no user-virtual-address dereferences | self-test `ep_format` (length > 32 refused); stack canary catches an unchecked length (mutant M23); the drain is chunked (`drain_chunks`) and wake-all goes through the reaper | ⚠️ messages bounded, no allocation, both queue-draining loops chunked (ADR 0010 stages 3-4); the work counter that measures W arrives at stage 6 |
| I-W1 | An IPC buffer counts as a writable alias for W^X | self-test `ep_ipcbuf_wx` | ✅ tested |
| I-E1 | Endpoint rendezvous: never both queues non-empty; FIFO; one sender and one receiver per rendezvous; no staged message outlives its send wait; all validation before any dequeue | `ipc_audit()` rule 7, self-tests `ep_fifo`, `ep_format`, `ep_capture` | ✅ tested |
| D1 | A device cannot DMA outside its assigned IOMMU domain | | planned (ADR 0001) |
| R1 | Cryptographic randomness is never returned before the generator is seeded | | planned (ADR 0004) |
| S1 | No physical frame is reused while any CPU may still cache a translation to it | self-test `vspace_teardown` (stale-TLB probe, single CPU) | ⚠️ single CPU tested; shootdown planned (ADR 0005) |
| B1 | Only signed kernels boot | | planned |

## Protections in place

| Protection | Where | Invariant |
|---|---|---|
| W^X kernel image: text R-X, rodata R--, data RW- | `kernel/linker.ld` | K1, K2 |
| CR0.WP | `arch/x86_64/cpu.c` | K1 |
| NX (EFER.NXE), boot refused without it | `cpu.c` | K2 |
| SMEP / SMAP | `cpu.c` | K3 |
| UMIP (user space can't read GDT/IDT addresses) | `cpu.c` | |
| Unmapped guard page below every kernel stack | `arch/x86_64/entry.S`, `paging.c` | K4 |
| `-fstack-clash-protection`, 1 KiB frame limit, no VLAs | Makefile | K4 |
| Separate IST stacks for #DF and NMI | `gdt.c` | K4 |
| Stack canaries, random per boot, low byte zero | `core/stack_protector.c` | K5 |
| Zero on allocation and on free | `core/pmm.c` | M1 |
| Untyped memory and retype; no kernel allocation on a task's behalf | `core/untyped.c` | O1–O11 |
| One sanitised entry state for every vector, SYSCALL and NMI/#DF/#MC | `arch/x86_64/isr.S`, `user_entry.S`, `user.c` | T-E, T-J |
| Validated return path; SYSRET only when provably safe | `arch/x86_64/user.c` | T-C, T-F |
| Eager FPU/vector context switching; new threads start at the initial state | `arch/x86_64/cpu.c`, `thread.c` | T-D |
| User memory only through exact-recovery copy routines; distinct address types | `core/uaccess.c`, `arch/x86_64/copy_user.S`, `include/tarka/types.h` | K3, T-G, T-L, T-M |
| Every kernel stack guarded, painted and budgeted; no unbounded recursion | `arch/x86_64/stacks.c`, `core/untyped.c` | T-K |
| Kernel half of every address space supervisor-only at the PML4 level | `arch/x86_64/vspace.c` | U2 |
| Address spaces: user-half-only mapping, global W^X across aliases, shadow back-pointers, full user-TLB flush on every removal | `arch/x86_64/vspace.c` | U2, U3, U5, S1 |
| Double-free and foreign-frame rejection | `pmm.c` | M2 |
| Zero-initialised automatic variables | `-ftrivial-auto-var-init=zero` | M1 |
| No I/O port access from ring 3 by default | TSS I/O bitmap | |
| Bounds-checked `kvsnprintf` | `lib/printf.c` | |
| CET `endbr64` landing pads | `-fcf-protection=full` | |
| Bootloader pinned by commit hash | `scripts/fetch-limine.sh` | B1 (partial) |
| CI actions pinned by commit SHA | `.github/workflows/ci.yml` | |

## Known limitations

- No KASLR yet: the kernel links at a fixed address.
- Panic and fault messages print kernel addresses. Fine on the serial
  console during development; release builds must redact them once user
  space can read logs.
- The stack canary is seeded directly from RDRAND, with a weak TSC fallback.
  ADR 0004 replaces this with a proper entropy subsystem.
- The kernel is single-CPU. ADR 0005 defines the SMP model.
- **No timer preemption yet (ADR 0008, T36):** a user thread runs until it
  yields, faults, exits or makes a blocking call. Only the kernel's own
  test programs run in ring 3 until ADR 0010 stage 6 adds the quantum
  timer. Stage 5 put every scheduling transition behind one checked
  primitive and gave each CPU an idle thread, so the timer has a single
  place to preempt from.
- **Hardware behaviour not observable under QEMU's software emulator**,
  still to be confirmed on real hardware or KVM: #GP/#SS (rather than
  #PF) for non-canonical user accesses, #XM and #AC from user mode, an NMI
  arriving while user code runs, and the Intel-specific SYSRET fault
  (prevented by construction). The tests accept the emulator's outcome
  for these and print a note.
- SMEP and SMAP are required: the kernel refuses to boot without them, as
  it does without NX.

## Roadmap

1. **Kernel object model** (ADR 0003): IPC, then the scheduler with
   timer preemption (the derivation tree, revocation, Untyped, retype,
   address spaces, threads, user entry and syscalls are done). Invariants
   C6, U1.
2. **Entropy** (ADR 0004). Invariant R1.
3. **SMP** (ADR 0005). Invariant S1.
4. **Fuzzing:** machine-readable syscall descriptions, coverage-guided
   syscall fuzzing in CI, KASAN-style checks in debug builds.
5. **Devices:** IOMMU domains and DMA ownership through capabilities
   (ADR 0001). Invariant D1.
6. **Boot chain:** KASLR, UEFI Secure Boot, TPM measured boot. Invariant B1.
7. **Trust and update infrastructure** (see below).
8. **Process:** security advisories, CVE handling, coordinated disclosure.

### Trust and update infrastructure

The update system is part of the trusted computing base: it has the power
to replace every other component.

```text
reproducible build → signed artifact → signed metadata → verified download
  → atomic install into inactive slot → A/B boot → health check → commit or roll back
```

Requirements:
- **Reproducible builds,** so anyone can verify that a signed binary matches its source.
- **TUF-style metadata:** separate root, targets, snapshot and timestamp
  roles; threshold signatures; expiry, to stop freeze attacks.
- **Signing-key model:**
  - the root key is offline, ideally held in hardware and split across
    several holders
  - online keys are short-lived and rotatable
  - a compromised online key can be revoked by the root
- **Anti-rollback:** monotonic version counters anchored in TPM or secure
  storage; the A/B fallback never boots below the counter.
- **Health checks** decide whether a new slot is committed. Failure falls
  back to the previous slot automatically.

## Reporting a vulnerability

Until a dedicated channel exists, please report vulnerabilities privately
through GitHub's "Report a vulnerability" (Security tab) rather than in a
public issue.
