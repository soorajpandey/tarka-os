# ADR 0003: Kernel object model (capabilities, memory, address spaces, IPC, threads)

Status: accepted

## Context

Capabilities, address spaces, shared memory, IPC and threads all depend on
each other. If they're designed one at a time, each later piece has to work
around the earlier ones; that's how ambient authority and lifetime bugs
creep back in. This ADR designs them together and answers the open
questions from ADR 0002.

Prior art: seL4 (capability derivation tree, untyped retyping, endpoints),
Fuchsia Zircon (handles, VMOs), QNX (synchronous message passing).

## Principles

1. **No kernel allocation without donated memory.** After boot, the kernel
   never allocates memory on a task's behalf from a global pool. Every
   kernel object is carved out of an `Untyped` capability the caller holds.
   A task therefore cannot exhaust kernel memory beyond what it was given,
   which closes a denial-of-service path.
2. **All authority is a capability,** including the right to set a
   priority, to handle a thread's faults, or to receive an interrupt.
3. **Every object has exactly one lifetime rule:** it lives while a
   capability to it exists, and is destroyed with its last capability.
4. **No stale references:** the kernel never holds a raw pointer to an
   object that can outlive it. Internal references are capabilities stored
   in kernel-owned slots, so they are found and cleaned up by revocation.

## Kernel objects

| Object | Purpose |
|---|---|
| `Untyped` | A power-of-two block of physical memory; can be retyped into other objects |
| `Frame` | A mappable 4 KiB (later 2 MiB) page |
| `PageTable` | One page-table level; carved from Untyped by the caller |
| `AddressSpace` | Root page table (PML4) plus its address-space ID |
| `CNode` | A table of capability slots; a cspace is a tree of CNodes |
| `Thread` | Registers, cspace root, address-space root, IPC buffer, fault endpoint, priority |
| `Endpoint` | Synchronous IPC rendezvous point |
| `Notification` | Asynchronous word of signal bits (IRQs, events) |
| `Reply` | One-shot capability to answer a `call` |
| `IrqHandler` | Right to acknowledge one interrupt line and bind it to a Notification |
| `IoPorts` | A range of x86 I/O ports |
| `DmaDomain` | An IOMMU domain plus the frames mapped into it (ADR 0001) |

## Capabilities

### Representation

A slot holds `{type, rights, object, badge}` plus derivation-tree links.
All of it lives in kernel memory (CNodes are kernel objects); user space
only ever names a slot by index. Corrupting user memory therefore cannot
forge or alter a capability.

### Creation

- At boot, the kernel gives the initial task an `Untyped` capability for
  every usable memory region, plus device capabilities (IRQs, I/O ports,
  device frames). The kernel creates nothing else.
- `retype(untyped, type, count, dest_cnode, dest_slot)` carves new objects
  out of an Untyped block. The new capabilities are children of the
  Untyped in the derivation tree.
- An Untyped region with live children cannot be retyped again until those
  children are revoked. This rule stops the same physical memory being both
  a kernel object and a user-mappable frame.

### Derivation, duplication, delegation

- `derive` (implemented) copies a capability with rights narrowed to
  `source & mask`. It needs GRANT on the source and can set a badge only
  once.
- The copy is recorded as a child of the source in the **capability
  derivation tree (CDT)**. The tree is stored as a doubly linked pre-order
  list with depths, as in seL4, so revocation needs no extra allocation.
  *Amended by ADR 0010 (SC20, SC20a):* the list is a balanced-bracket
  list (an open and a close element per record, no depths), so deleting
  a record and each revocation step are O(1). `struct cap` is 128 bytes.

### Revocation and destruction

- `revoke(cap)` deletes every descendant of `cap` in the CDT and keeps
  `cap` itself. This is how a supervisor takes back everything it
  delegated. *Amended by ADR 0010 (SC22-SC22b):* it runs to completion as
  a thread continuation, in bounded chunks, with derivation into the
  subtree frozen until it finishes.
- `delete(cap)` removes one capability. When it was the last capability to
  its object (no CDT siblings or ancestors referring to the same object),
  the object is destroyed. *Amended by ADR 0010 (SC19a, SC23):* "last"
  is decided by the object's live-record count, never by CDT position,
  and objects whose teardown is not constant-time are handed to the
  reaper:
  - **Thread:** stopped and removed from every queue.
  - **Endpoint:** blocked senders and receivers are woken with `T_EDEAD`.
  - **Frame:** unmapped from every address space (with TLB shootdown,
    ADR 0005) before its memory can be reused.
  - **AddressSpace:** all mappings removed and the address-space ID
    released.
- Revoking an Untyped destroys every object carved from it and returns the
  memory to the Untyped, scrubbed.
- Revocation of long subtrees is **preemptible and restartable**: the
  operation records progress in the tree and resumes, so a huge subtree
  can't freeze the system.

### Stale capabilities

User space can't hold a stale capability: revocation removes the slot
contents, and a later use of that index fails with `T_EBADCAP`. Kernel
internal references (a thread's fault endpoint, its IPC buffer frame) are
stored as capability copies in the Thread object, so revocation reaches
them too.

### Capability transfer over IPC

- A message may carry up to 3 capabilities. Sending them requires GRANT on
  the endpoint capability used, so a client that was not trusted to pass
  capabilities cannot push any.
- The receiver opts in by naming an empty receive slot before it waits.
  Without one, capabilities are not transferred and the receiver is told
  so. Nothing lands in a cspace uninvited.
- **Unwrapping:** if a transferred capability is a badged capability to an
  endpoint the receiver itself serves, the kernel delivers only the badge.
  A server can recognise its own objects without trusting the client's
  claims.
- The receiver can ask the kernel for a received capability's type, rights
  and badge (`cap_identify`) and must check them before using it. Servers
  never act on a capability's type as described by the client in the
  message body.

### Lookup cost

A cspace is a tree of CNodes addressed by a 64-bit slot path, resolved a
bounded number of levels at a time (a fixed maximum depth), so the cost of
a lookup is bounded and independent of cspace size.

## Address spaces and memory

- **Kernel half:** shared by all address spaces. Its top-level entries are
  created at boot and never change, so they can be copied into each new
  address space once.
- **User half:** below `0x0000_8000_0000_0000`. Every mapping request is
  checked for canonical form and user range. A task can never map kernel
  memory (U2).
- `map(frame, address_space, vaddr, rights)`:
  - the mapping's rights are at most the frame capability's rights
  - **W and X are never both granted** (U3)
  - missing page-table levels are supplied by the caller as `PageTable`
    objects carved from its own Untyped (principle 1)
- **One mapping per frame capability.** To share a frame, derive another
  capability and map that one. Unmapping and revocation can then find
  every mapping exactly.
- **Shared memory** is a frame capability delegated through IPC, with rights
  narrowed as needed, e.g. read-only for a consumer. Bulk data moves this
  way; IPC messages stay small.
- **User access from the kernel** happens only through `copy_from_user` /
  `copy_to_user`:
  - the user range is validated first
  - access is enabled with SMAP `stac` and disabled with `clac`
  - faults inside the copy return `T_EFAULT` rather than panicking
  - each user value is fetched exactly once; the kernel copies it, then
    validates the copy, which prevents double-fetch races

## IPC

- **Endpoints, synchronous rendezvous.** `send`, `recv`, `call` (send, then
  wait on a fresh Reply), `reply_recv` (the server fast path).
- **Message format:** a label plus up to 4 words in registers (the fast
  path), with up to 64 more words in the thread's IPC buffer, a frame the
  thread registered at setup. Messages are copied by the kernel directly
  between the two threads, never via kernel heap.
- **Notifications** carry asynchronous events and IRQs: a word of bits,
  OR-ed in by `signal` and atomically read and cleared by `wait`.
  Signalling never blocks, so a slow consumer cannot stall a producer.
- **Server death:** destroying a Reply capability, for example because the
  server died, wakes the caller with `T_EDEAD`. A dying client invalidates
  its pending Reply; the server's later `reply` fails harmlessly.
- **Denial of service:**
  - `call` and `recv` take an optional timeout
  - the structural rule is that a server never `call`s a less-trusted
    component. It only replies, so a malicious client cannot block it.
- **Malformed messages:** the kernel validates only structure (lengths,
  capability counts, slots). Content validation is the receiver's job, and
  every server treats message contents as hostile input. Servers are
  fuzzing targets.
- **Priority:** fixed-priority preemptive scheduling. Servers run at a
  priority at least as high as their clients (a priority ceiling), which
  avoids priority inversion without kernel inheritance. Scheduling
  contexts (seL4 MCS-style budgets) come later for real-time isolation.

## Threads and faults

- A Thread is configured through its capability: cspace root, address-space
  root, IPC buffer, fault endpoint, priority.
- A thread can only set priorities up to the maximum controlled priority
  stored in the Thread capability it uses, so priority is authority too.
- **Faults never panic the kernel** (U4). A user fault (page fault,
  invalid opcode, general protection) becomes a fault message sent to the
  thread's fault endpoint. The handler can fix the fault and resume the
  thread, restart it, or destroy it. A thread with no fault endpoint is
  suspended.
- **Server restart:** a supervisor holds the capabilities needed to
  recreate each server. When a server dies, its clients get `T_EDEAD`,
  the supervisor restarts it, and clients reconnect through the name
  service.
- Only faults in kernel mode panic. The kernel is the component we are
  least willing to restart.

## Invariants introduced

C5, C6, U1, U2, U3 and U4 in `docs/SECURITY.md`. Each gets a self-test
before its subsystem is considered done.

## Implementation order

1. CDT plus `revoke` in the existing flat cspace. **Done.**
2. Untyped and `retype`; boot hands all usable memory out as Untyped.
   **Done**, specified in detail by [ADR 0006](0006-untyped-retype.md).
3. Page-table management for the kernel half; AddressSpace, PageTable and
   Frame objects; `map` / `unmap`. **Done**, specified in detail by
   [ADR 0007](0007-address-spaces.md).
4. Thread objects, context switch, user-mode entry, syscall entry,
   `copy_*_user`. **Done**, specified in detail by
   [ADR 0008](0008-threads-user-entry-syscalls.md).
5. Endpoints, Reply, Notifications; capability transfer.
6. Fault delivery; the first user-space task (init); the first user-space
   driver (serial, behind `IoPorts`).

## Open questions

- Should Frame objects be able to come from device memory (MMIO)? Yes, but
  as a separate `DeviceFrame` type that is never retyped and never zeroed.
- Size limits for CNodes, and the maximum cspace depth.
