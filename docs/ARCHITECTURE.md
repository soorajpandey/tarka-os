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
   - enables CPU hardening (WP, NX, SMEP, SMAP, UMIP)
   - unmaps the stack guard pages
   - seeds the stack canary from RDRAND
   - initialises the physical frame allocator
4. In `make test` builds, the self-tests then run and the result is reported
   to QEMU through the isa-debug-exit device.

## Kernel subsystems

| Subsystem | File | State |
|---|---|---|
| Entry, stacks | `arch/x86_64/entry.S` | done |
| GDT, TSS, IST | `arch/x86_64/gdt.c` | done |
| Exceptions | `arch/x86_64/idt.c`, `isr.S` | done, all kernel faults panic |
| CPU hardening | `arch/x86_64/cpu.c` | done |
| Page-table inspection | `arch/x86_64/paging.c` | query and unmap only |
| Physical memory | `core/pmm.c` | bitmap allocator |
| Capabilities | `core/cap.c` | flat cspace; no revocation tree yet |
| Virtual memory, address spaces | | next |
| Threads, scheduler | | planned |
| Syscalls, IPC | | planned |
| APIC, timers, IRQ delivery | | planned |

## Conventions

- Fallible functions return 0 or a negative `T_E*` code and are marked
  `__must_check`.
- Kernel code never trusts sizes or indices from user space or the
  bootloader without checking them; size arithmetic is overflow-checked.
- Every new security property comes with a self-test.
