# Security model

## Principle

Tarka does not claim to be bug-free. It is designed to **minimise the
impact of vulnerabilities, detect failures early, and distribute verified
fixes quickly**.

## Threat model (initial)

In scope:
- Malicious or compromised user-space programs, including drivers and
  servers, trying to escalate privilege or read other tasks' data.
- Malicious devices or drivers using DMA (mitigated by the IOMMU, see ADR 0001).
- Tampered boot media or update packages (verified and measured boot,
  signed updates: planned).

Out of scope for now: physical attacks on RAM, side channels beyond the
standard Spectre/Meltdown mitigations, malicious firmware.

## Protections in place

Each item below is verified on every CI run by the boot-time self-tests
(`make test`).

| Protection | Where | What it prevents |
|---|---|---|
| W^X kernel image: text R-X, rodata R--, data RW- | `kernel/linker.ld` | code injection, patching kernel code or constants |
| CR0.WP | `arch/x86_64/cpu.c` | the kernel writing through read-only mappings |
| NX (EFER.NXE), boot refused without it | `cpu.c` | executing data |
| SMEP / SMAP | `cpu.c` | the kernel executing or touching user memory unintentionally |
| UMIP | `cpu.c` | user space reading GDT/IDT addresses (layout leak) |
| Stack canaries, random per boot, low byte zero | `core/stack_protector.c`, `-fstack-protector-strong` | stack buffer overflows overwriting return addresses |
| Unmapped guard page below every kernel stack | `arch/x86_64/entry.S`, `paging.c` | stack overflow silently corrupting adjacent memory |
| `-fstack-clash-protection` | Makefile | large frames jumping over a guard page |
| Stack frames limited to 1 KiB, no VLAs | Makefile | stack exhaustion |
| Separate IST stacks for #DF and NMI | `gdt.c` | handling a fault on a corrupt stack |
| Zero on allocation and on free | `core/pmm.c` | leaking stale data between owners |
| Double-free and foreign-frame rejection | `pmm.c` | allocator corruption |
| No padding in `struct cap` | `cap.h` | leaking kernel bytes through copied structs |
| Zero-initialised automatic variables | `-ftrivial-auto-var-init=zero` | uninitialised-stack info leaks |
| No I/O port access from ring 3 by default | TSS I/O bitmap | unprivileged hardware access |
| Bounds-checked `kvsnprintf` | `lib/printf.c` | format-buffer overflows |
| CET `endbr64` landing pads | `-fcf-protection=full` | indirect-branch hijacking (on CET hardware) |
| Bootloader pinned by commit hash | `scripts/fetch-limine.sh` | substituted bootloader binaries |
| CI actions pinned by commit SHA | `.github/workflows/ci.yml` | supply-chain attacks through moved tags |

Known limitations:
- No KASLR yet: the kernel links at a fixed address.
- Panic and fault messages print kernel addresses. Fine on the serial
  console during development; release builds must redact them once user
  space can read logs.
- RDRAND is the only good entropy source. Without it the canary seed falls
  back to the TSC, which is weak.

## Roadmap

1. **Now:** virtual memory manager, kernel heap with redzones, user
   address spaces, syscall entry with `copy_from_user` / `copy_to_user`
   (read each user argument exactly once, under SMAP `stac`/`clac`).
2. **IPC and servers:** endpoints, capability transfer, derivation-tree
   revocation, first user-space driver (serial) behind `CAP_IOPORT`.
3. **Fuzzing:** machine-readable syscall descriptions, syzkaller-style
   coverage-guided fuzzing in CI, KASAN-style checks in debug builds.
4. **Boot chain:** KASLR (relocatable kernel), UEFI Secure Boot, TPM
   measured boot, IOMMU setup.
5. **Updates:** TUF-style signed metadata (offline root key, threshold
   signing, key rotation, freeze protection), A/B slots with health checks,
   anti-rollback counters, reproducible builds.
6. **Process:** security advisories, CVE handling, coordinated disclosure.

## Reporting a vulnerability

Until a dedicated channel exists, please report vulnerabilities privately
through GitHub's "Report a vulnerability" (Security tab) rather than in a
public issue.
