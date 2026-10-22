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

Each invariant is a statement that must always hold. Each one is proven by
a self-test that runs in CI (`make test`), or is marked as planned until
the code it depends on exists. Adding a subsystem means adding its
invariants and tests here.

| ID | Invariant | Proof | Status |
|---|---|---|---|
| K1 | Kernel executable memory is never writable | self-test `w^x` (page-table walk); write injection faults | ✅ tested |
| K2 | Kernel data and read-only data are never executable | self-test `w^x` | ✅ tested |
| K3 | The kernel cannot execute or access user memory outside explicit copy routines | self-test `cpu_hardening` (SMEP/SMAP on) | ⚠️ enablement tested; copy routines planned |
| K4 | A kernel stack overflow faults; it never corrupts adjacent memory | self-test `stack_guards`; recursion injection gives a clean #DF | ✅ tested |
| K5 | A stack buffer overflow is detected before the function returns | self-test `cpu_hardening` (canary is random); smash injection panics | ✅ tested |
| M1 | Memory given to a new owner contains no data from a previous owner | self-test `pmm` (zero on alloc and free) | ✅ tested |
| M2 | The allocator never releases memory it does not own, and detects double frees | self-test `pmm` | ✅ tested |
| C1 | Rights can never increase through derivation | self-test `capabilities` | ✅ tested |
| C2 | Every capability use is checked for bounds, type and rights | self-test `capabilities` | ✅ unit; syscall path planned |
| C3 | A badge cannot change once set | self-test `capabilities` | ✅ tested |
| C4 | Capability records contain no uninitialised bytes | `_Static_assert` in `cap.h` | ✅ compile time |
| C5 | Revoking a capability removes every capability derived from it | | planned (ADR 0003) |
| C6 | Deleting the last capability to an object frees the object | | planned (ADR 0003) |
| U1 | A task cannot obtain a capability it was not explicitly given | | planned (ADR 0003) |
| U2 | A task cannot map kernel memory or another task's memory without a frame capability | | planned (ADR 0003) |
| U3 | No user mapping is both writable and executable | | planned (ADR 0003) |
| U4 | A user-space fault never panics the kernel | | planned (ADR 0003) |
| D1 | A device cannot DMA outside its assigned IOMMU domain | | planned (ADR 0001) |
| R1 | Cryptographic randomness is never returned before the generator is seeded | | planned (ADR 0004) |
| S1 | No physical frame is reused while any CPU may still cache a translation to it | | planned (ADR 0005) |
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

## Roadmap

1. **Kernel object model** (ADR 0003): capability derivation tree and
   revocation, untyped memory, address spaces, threads, IPC. Invariants
   C5–C6, U1–U4.
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
