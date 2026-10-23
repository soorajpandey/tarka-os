# Tarka OS

A security-first microkernel operating system for x86-64, written in C.

Tarka's goal is not to claim it has no bugs. It is to **limit the damage any
single bug can do, detect failures early, and ship verified fixes quickly**.
The design choices that are hardest to retrofit are made from the first
commit:

- **Microkernel.** The kernel provides only address spaces, threads, IPC,
  capabilities and interrupt delivery. Drivers, filesystems and the network
  stack run as isolated user-space servers. See
  [ADR 0001](docs/adr/0001-microkernel.md).
- **Capabilities, no ambient authority.** A task can only act on an object
  it holds an unforgeable capability to. Rights can be narrowed when
  delegated, never widened. See [ADR 0002](docs/adr/0002-capabilities.md).
- **Hardened from boot.** W^X kernel image, guarded stacks, stack
  canaries, SMEP/SMAP/UMIP, zeroed memory on allocation and free, all
  verified by boot-time self-tests in CI.

Status: early. The kernel boots, enables CPU hardening, hands all memory
out as Untyped capabilities, and creates kernel objects only by retyping
memory the caller holds. Capabilities form a derivation tree with
revocation. Address spaces are built from caller-owned page tables, with
W^X enforced across every alias of a physical frame. Threads run in
ring 3 behind a validated entry and exit boundary, and user programs act
through 19 capability-checked syscalls. There is no IPC or timer
preemption yet, so only the kernel's own test programs run in user mode. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the roadmap and
[docs/SECURITY.md](docs/SECURITY.md) for the threat model and the security
invariants, each with the test that proves it.

## Building

Requirements: `gcc`, GNU `ld`, `make`, `git`, `xorriso`, `qemu-system-x86_64`.

```sh
make          # build build/release/tarka.iso (fetches the pinned Limine bootloader)
make run      # boot in QEMU, serial console on stdio
make test     # build with self-tests and run them in QEMU; exit status reflects the result
make clean
```

On Debian/Ubuntu: `sudo apt-get install build-essential xorriso qemu-system-x86`.

## Layout

```
kernel/
  arch/x86_64/       entry, GDT/TSS, IDT, CPU hardening, paging, serial
  core/              kmain, panic, physical memory, capabilities, self-tests
  lib/               freestanding string and printf
  include/tarka/     kernel headers
  third_party/limine limine.h boot protocol header (0BSD)
docs/                architecture, security model, decision records
scripts/             bootloader fetch (pinned by commit), QEMU test runner
```
