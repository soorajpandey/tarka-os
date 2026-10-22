# ADR 0004: Kernel entropy and random number generation

Status: accepted

## Context

The kernel currently seeds the stack canary directly from RDRAND, falling
back to the TSC. That works for a canary but is not a design. Upcoming
users need randomness with different strength:

| User | Needs |
|---|---|
| Stack canaries, KASLR, heap randomisation | Unpredictable at boot; available immediately |
| Cryptographic keys, update verification nonces, session secrets | Full cryptographic strength; must never be guessable |

Relying on a single hardware source is also a risk. RDRAND can fail, can
be missing, has had firmware bugs (e.g. returning all-ones after resume on
some AMD parts), and cannot be audited.

## Decision

### Sources

All available sources are mixed in, never just one:

1. **RDSEED** (true-random seed source) when present; **RDRAND** otherwise.
2. **TSC jitter** sampled around boot-time work.
3. **Interrupt timing** (cycle counter at each interrupt) once interrupts
   are enabled.
4. **Firmware or TPM randomness** when available (UEFI `EFI_RNG_PROTOCOL`,
   TPM `GetRandom`).
5. **A stored seed file,** later: read at boot, immediately replaced, so no
   two boots reuse it.

### Structure

```text
sources ──► entropy pool (BLAKE2s accumulator, credited bits)
                │  reseed (≥256 bits credited)
                ▼
          ChaCha20 DRBG, one per CPU, with fast key erasure
                │
                ├── random_early()   best effort, never blocks
                └── random_bytes()   cryptographic; refuses until seeded
```

- **Pool:** inputs are hashed into a BLAKE2s state. Hashing means an
  attacker-controlled input cannot reduce the entropy of the others.
- **Generator:** ChaCha20, rekeyed after every request (fast key erasure),
  so a later memory disclosure cannot recover earlier outputs. Reseeded
  from the pool periodically and whenever fresh entropy is credited.
- **Per-CPU instances** remove locking from the hot path (ADR 0005).

### States

| State | `random_early()` | `random_bytes()` |
|---|---|---|
| `UNSEEDED` | Returns the best mix available | Fails with `T_EAGAIN` (or blocks, at the caller's choice) |
| `SEEDED` (≥256 bits credited) | Full strength | Full strength |

Invariant **R1**: `random_bytes()` never returns output in `UNSEEDED`.

### Crediting policy

- Every source is always mixed in. Crediting decides only when the
  generator counts as seeded.
- CPU hardware RNG output is credited by default, controlled by a
  build-time policy option. Builds for high-assurance targets can turn it
  off and wait for other sources.
- TSC jitter and interrupt timing are credited conservatively: at most
  1 bit per sample, and only after health tests pass.

### Health tests

- RDRAND/RDSEED: retry on carry-flag failure (up to 10 times); reject
  all-zeros and all-ones words; a repetition-count test catches stuck
  output.
- Jitter sources: the repetition-count and adaptive-proportion tests from
  NIST SP 800-90B.
- A failing source is disabled for the rest of the boot and the failure is
  logged.

### Other events

- The canary moves from direct RDRAND to `random_early()`.
- On VM snapshot restore (when a VM generation ID changes), the kernel
  reseeds immediately, so two restored copies don't produce the same
  stream.

## Tests

- Known-answer tests for ChaCha20 (RFC 8439 test vectors) and BLAKE2s
  (RFC 7693), run at boot in self-test builds.
- State machine: `random_bytes()` fails before seeding and succeeds after.
- Fast key erasure: the generator key changes after every request.
- Health tests: a simulated stuck source is rejected.

## Consequences

- About 1–2k lines of new kernel code (BLAKE2s, ChaCha20, pool, health
  tests). It is security-critical and must be reviewed and fuzzed
  accordingly.
- Callers must choose explicitly between `random_early()` and
  `random_bytes()`. That choice is the point of the design.
