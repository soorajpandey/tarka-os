# Architecture

## Overview

```text
 ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐
 │   apps    │ │  fs srv   │ │  net srv  │ │  drivers  │   user space (ring 3)
 └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └─────┬─────┘   each its own address space,
       │   IPC via capabilities (endpoints)       │         holding only the caps it needs
 ══════╪═════════════╪═════════════╪═════════════╪══════
 ┌─────┴─────────────┴─────────────┴─────────────┴─────┐
 │ microkernel: address spaces · threads · IPC ·       │   kernel (ring 0)
 │              capabilities · IRQ delivery · memory   │
 └──────────────────────────┬──────────────────────────┘
                            │ IOMMU restricts device DMA
                        hardware
```

## Boot flow

1. The Limine bootloader (pinned commit) loads `tarka.elf` into the higher
   half and maps each ELF segment with its own permissions (W^X from the
   first instruction).
2. `_start` (`arch/x86_64/entry.S`) switches to the kernel's guarded boot
   stack and calls `kmain`.
3. `kmain` (`core/main.c`):
   - brings up the serial console
   - validates the bootloader responses
   - loads the GDT/TSS (IST stacks for #DF and NMI) and the IDT
   - enables CPU hardening (WP, NX, SMEP, SMAP, UMIP; NX, SMEP and SMAP
     are mandatory) and eager FPU/XSAVE state
   - sets up per-CPU data through the GS base
   - unmaps the stack guard pages
   - seeds the stack canary from RDRAND
   - initialises the physical frame allocator
   - hands all usable memory except an 8 MiB kernel reserve to root
     Untyped capabilities
   - enables the local APIC (mapping its MMIO page if needed)
   - records the master PML4 and clears the user bit on every kernel-half
     top-level entry
   - registers thread finalisers, masks the legacy PIC and programs the
     SYSCALL MSRs
4. In `make test` builds, the self-tests then run and the result is reported
   to QEMU through the isa-debug-exit device.

## Kernel subsystems

| Subsystem | File | State |
|---|---|---|
| Entry, stacks | `arch/x86_64/entry.S` | done |
| GDT, TSS, IST | `arch/x86_64/gdt.c` | done |
| Exceptions | `arch/x86_64/idt.c`, `isr.S` | done, all kernel faults panic |
| CPU hardening | `arch/x86_64/cpu.c` | done |
| Page-table inspection | `arch/x86_64/paging.c` | query, kernel guard pages |
| Physical memory | `core/pmm.c` | bitmap allocator |
| Capabilities | `core/cap.c`, `core/objmeta.c`, `core/reaper.c`, `core/cont.c` | flat cspaces; 128-byte records in a bracket-list derivation tree across cspaces; resumable revocation; per-object live-record, rights and frame-alias counts (finality by count only), carving back-pointers, derivation freeze check; never-reused serials; dying objects torn down by the reaper; finalisers |
| Untyped memory, retype | `core/untyped.c` | done ([ADR 0006](adr/0006-untyped-retype.md)): boot handoff, Frame/Endpoint/CNode/Untyped objects |
| Address spaces, page tables | `arch/x86_64/vspace.c` | done ([ADR 0007](adr/0007-address-spaces.md)): install/map/unmap/protect, W^X across aliases, teardown in any order |
| Threads, user entry, syscalls, faults | `arch/x86_64/thread.c`, `user.c`, `isr.S`, `user_entry.S`, `core/syscall.c`, `core/uaccess.c` | done ([ADR 0008](adr/0008-threads-user-entry-syscalls.md)): ring 3 runs; 28 syscalls; thread state changes through `core/sched.c` (below) |
| Local APIC | `arch/x86_64/lapic.c` | x2APIC or xAPIC (MMIO); self-IPI, self-NMI, one-shot timer |
| IPC, notifications, fault IPC | `core/ipc.c`, `endpoint.c`, `notification.c` | done ([ADR 0009](adr/0009-ipc.md)): send/recv/call/reply/reply_recv, capability transfer, badges, notifications, fault delivery |
| Scheduler with timer preemption, bounded kernel work | | specified ([ADR 0010](adr/0010-scheduler.md)); stages 1-4 done (bracket-list CDT; counters, frame metadata table and creation gates; serials, bounded page-table detach, dying objects and the reaper; thread continuations: revocation with the derivation freeze, the chunked receive drain, the retype limit; scheduler plumbing in `core/sched.c`: one primitive per transition, sole writer of thread state, run queue and `current` (gate `scripts/check-sched.sh`), per-CPU idle thread so `current` is never NULL, halt-and-wake idle loop); stage 6 (timer and fairness) next. Gate for untrusted code (ADR 0008 T36) |
| Entropy, random generator | | planned ([ADR 0004](adr/0004-entropy.md)) |
| SMP, APIC, TLB shootdown | | planned ([ADR 0005](adr/0005-smp.md)) |
| IOMMU, DMA domains | | planned ([ADR 0001](adr/0001-microkernel.md)) |

## Fixed kernel memory

Memory the kernel takes for itself at boot, before handing the rest to
user space as Untyped:

| What | Size | Notes |
|---|---|---|
| Frame bitmaps (`core/pmm.c`) | 2 bits per page | `used` and `managed` |
| Frame metadata table (`core/objmeta.c`) | 32 bytes per page: 2,044 KiB at 256 MiB of RAM (0.8%), measured at boot | ADR 0010 SC18; 24 bytes per page (1.5 MiB) at stage 2, grown at stage 3 to hold every object's serial (revision 3d, D3) |
| Kernel reserve | 8 MiB (`KERNEL_RESERVE_FRAMES`) | kernel page tables, stacks and other `pmm_alloc()` use |

Per-object kernel state lives in memory the object's creator supplied
from an Untyped, such as a 128-byte capability record or a 128-byte
endpoint. It is not part of this fixed cost.

## Design records

| ADR | Decision | Status |
|---|---|---|
| [0001](adr/0001-microkernel.md) | Microkernel; DMA ownership through the IOMMU | accepted |
| [0002](adr/0002-capabilities.md) | Capabilities, no ambient authority | accepted |
| [0003](adr/0003-kernel-object-model.md) | Kernel object model: capabilities, memory, address spaces, IPC and threads designed together | accepted |
| [0004](adr/0004-entropy.md) | Entropy pool and ChaCha20 random generator | accepted |
| [0005](adr/0005-smp.md) | SMP: big kernel lock first, per-CPU state, TLB shootdown | accepted |
| [0006](adr/0006-untyped-retype.md) | Untyped memory, retype, lifetime and reclamation (precise rules O1–O11) | accepted |
| [0007](adr/0007-address-spaces.md) | Address spaces, page tables, mapping authority, W^X and aliasing, unmapping, TLB invalidation (rules V1–V15) | accepted |
| [0008](adr/0008-threads-user-entry-syscalls.md) | Threads, context, user entry validation, syscall boundary, user-memory access, user faults, unified kernel entry, stack budget (rules T1–T36) | accepted |
| [0009](adr/0009-ipc.md) | IPC: endpoints, call/reply, notifications, capability transfer, fault IPC, liveness and atomicity (rules P1–P76, P6a, P23a, P27a) | accepted |
| [0010](adr/0010-scheduler.md) | Scheduler, timer preemption, bounded kernel work: idle threads, continuations, reaper, bracket-list CDT, maintained counters, frozen revocation (rules SC1–SC31, F1–F7, R0–R9) | accepted |

Security invariants and the tests that prove them are listed in
[SECURITY.md](SECURITY.md#security-invariants).

## Conventions

- Fallible functions return 0 or a negative `T_E*` code and are marked
  `__must_check`.
- Kernel code never trusts sizes or indices from user space or the
  bootloader without checking them; size arithmetic is overflow-checked.
- Every new security property comes with a self-test.
- A test is accepted only once it is shown to tell the property apart
  from plausible broken implementations (mutation testing). A mutant
  counts as killed only by a behavioural failure of the suite as it runs.
  A build failure, a harness failure, or an assertion inside the mutated
  code alone does not count (ADR 0010 SC31.6).
