# ADR 0002: Capability-based access control

Status: accepted

## Context

Unix-style permissions are *ambient authority*: a process carries its user
ID everywhere, and any code running in it can use that authority on any
object the user can reach. This is how confused-deputy attacks and
over-privileged services happen, and it makes sandboxing a separate,
bolted-on mechanism.

## Decision

All authority in Tarka is expressed as capabilities:

- A capability is a kernel-held record `{type, rights, object, badge}`
  (`kernel/include/tarka/cap.h`). User space refers to capabilities only by
  slot index in its own capability space (cspace) and can never read or
  forge the record.
- Every syscall that acts on an object takes a slot index and goes through
  `cap_lookup()`, which checks bounds, type and rights. This is the single
  choke point for authorisation.
- `cap_derive()` copies a capability with rights narrowed to
  `source & mask`. Rights are monotonic: they can never be widened.
  Copying requires the `GRANT` right on the source.
- Badges let a server tell its clients apart. A badge is set once at
  derivation and cannot be changed afterwards.
- There is no root user and no global object namespace that bypasses the
  cspace. The initial task receives capabilities to all resources and
  distributes narrowed copies.

## Not yet implemented

Designed in [ADR 0003](0003-kernel-object-model.md):

- **Derivation tree and revocation.** Revoking a capability must also
  revoke everything derived from it.
- **Object lifetime.** When the last capability to an object is deleted the
  object must be freed, or the system leaks memory. This needs reference
  counts or the derivation tree.
- cspaces as kernel objects (`CAP_CNODE`) with multi-level lookup.

## Consequences

Sandboxing, driver isolation and IPC access control are the same
mechanism. A service can be given exactly the authority it needs, and code
review can answer "what can this component do?" by listing its capabilities.
