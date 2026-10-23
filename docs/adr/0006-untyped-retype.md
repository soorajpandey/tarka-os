# ADR 0006: Untyped memory, retype, lifetime and reclamation

Status: accepted

Refines ADR 0003, step 2. This document is the specification; the code in
`kernel/core/untyped.c` implements it and `kernel/core/selftest.c`
(`untyped_*` tests) checks it.

## Terms

- **Untyped**: a capability to a naturally aligned, power-of-two range of
  physical memory, `[base, base + 2^bits)`, with `12 ≤ bits ≤ 47`. It
  holds a **watermark**: the offset below which the range has been handed
  out.
- **Retype**: creating new kernel objects (or smaller Untypeds) inside an
  Untyped's range. The new capabilities become its children in the
  capability derivation tree (CDT).
- **Object**: anything created by retype. Its physical range is
  `[paddr, paddr + 2^size_bits)`. The capability's `object` word encodes
  both: `paddr | size_bits` (the low 6 bits are free because every object
  is at least 64 bytes and naturally aligned).

## Rules

### Ownership

**O1. Every physical frame has exactly one owner.** At boot, every usable
frame is owned either by the kernel's own allocator (`pmm`, a fixed
reserve for kernel-internal use) or by exactly one root Untyped. Frames
handed to Untypeds are marked used in `pmm` and can never be allocated
by it. Inside an Untyped, every byte is either below the watermark
(owned by exactly one child) or above it (free in that Untyped).

**O2. An Untyped capability cannot be copied.** `cap_derive` on an
Untyped fails with `T_EINVAL`. Memory authority is delegated only by
retyping into child Untypeds, which cover disjoint subranges. So the
CDT below an Untyped *is* the physical-ownership tree. There is no
second tree to keep in sync.

### Retype

**O3. Retype never creates authority from nothing.** It requires:
- an Untyped capability with the WRITE right in the caller's cspace
- free space in that Untyped

There is no kernel allocator behind it. If the Untyped is full, retype
fails with `T_ENOMEM`.

**O4. Retype is deterministic.** It runs these steps in order and
modifies nothing until every check has passed:

1. Look up the Untyped (type `CAP_UNTYPED`, right WRITE).
2. Check the object type is retypeable: `UNTYPED`, `FRAME`, `ENDPOINT`,
   `CNODE`, `ADDRESS_SPACE`, `PAGE_TABLE` or `THREAD`.
3. Compute the object size:

   | Type | `size_bits` argument | Object size |
   |---|---|---|
   | `FRAME` | must be 0 | 4 KiB |
   | `ENDPOINT`, `NOTIFICATION` | must be 0 | 128 B (64 B before ADR 0010 stage 2, which added their metadata) |
   | `CNODE` | number of slots is `2^size_bits`, with 1 ≤ `size_bits` ≤ 12 | 2^(`size_bits`+6) B |
   | `UNTYPED` | the child's size, with 12 ≤ `size_bits` < parent's bits | 2^`size_bits` B |
   | `ADDRESS_SPACE`, `PAGE_TABLE` | must be 0 | 8 KiB (ADR 0007) |
   | `THREAD` | must be 0 | 8 KiB (ADR 0008) |

4. Check that `count` is between 1 and 256.
5. Check that the destination slots `dst .. dst+count-1` exist (with
   overflow-checked arithmetic) and are all empty.
6. **Reset rule:** if the Untyped has no children, set its watermark to 0.
7. Set `start = align_up(watermark, object size)` and
   `end = start + count × size`, overflow-checked. If `end` exceeds the
   Untyped's size, fail with `T_ENOMEM`.
8. Zero `[start, end)`, except when creating child Untypeds: they hold no
   data, and their own objects are zeroed when created.
9. Install one capability per object as a child of the Untyped, and set
   the watermark to `end`.

**O5. Objects never overlap.** Because of steps 6 and 7, each new object
starts at or above the watermark, is aligned to its own size, and lies
within the Untyped. The watermark only moves backwards (step 6) when no
child exists, so nothing below it can still be in use.

**O6. Objects are zeroed when created** (step 8). No object ever exposes
a previous owner's data, whatever happened to the memory before.

### Lifetime and reclamation

**O7. Memory becomes reusable exactly when its Untyped has no children.**
*Amended by ADR 0010 (SC23):* and its dying count is 0, so memory of an
object still being torn down by the reaper is never reused; a retype
that would reset the watermark meanwhile returns `T_EAGAIN`.
Deleting one object out of many does *not* make its bytes reusable; the
watermark only resets once the last child is gone. This is the seL4
model: O(1), with no per-object free lists hidden in the kernel. For
fine-grained reuse, split an Untyped into small child Untypeds and retype
one object from each. Deleting that object frees that child.

**O8. An Untyped with children cannot be deleted.** *Amended by ADR 0010
(SC23):* nor while its dying count is nonzero. `cap_delete` on it
fails with `T_EBUSY`; revoke it first. Otherwise its children would be
re-parented above it and the memory it covered would lose its owner.
`cspace_destroy` revokes Untypeds before deleting them.

**O9. Revoking an Untyped destroys everything created from it,** however
deeply nested: objects, child Untypeds, the grandchildren of those, and
every capability derived from any of them, in any cspace. Afterwards the
Untyped has no children, so its whole range is reusable (O7).

### Authoritative and internal references

**O10.** A reference to a retyped object is one of:

- **Authoritative:** a capability. Only capabilities keep an object
  alive. An object is finalised when its last capability is deleted, and
  its memory is reusable only after that (O7).
- **Internal:** any other kernel pointer to the object, such as a thread
  in a scheduler queue, a waiter on an endpoint, or a cached CNode view.
  Internal references must be removed by the object's **finaliser**, which
  runs before the deletion completes. The kernel never caches anything
  that the finaliser cannot clear. For example, a CNode is always
  accessed through a fresh `cnode_view()` derived from its capability,
  never through a stored pointer.

The kernel is non-preemptible (ADR 0005, big lock), so a finaliser runs to
completion before any retype can observe the freed range. Preemptible
finalisers will need seL4-style "zombie" capabilities that stay in the CDT
until the finaliser finishes; that is required before any finaliser is
allowed to be preemptible.

**O11.** Finalisers may delete other capabilities. A CNode's finaliser
deletes every capability stored in it. Revocation therefore never relies
on a CDT pointer it saved before a finaliser ran: if more than one
capability was deleted in a step, it re-finds the end of the subtree.

## Boot

`untyped_boot()` runs right after `pmm_init()`:

- It keeps `KERNEL_RESERVE` frames in `pmm` for the kernel's own use.
- It repeatedly takes the largest naturally aligned, entirely free block
  (up to 1 GiB) from `pmm`, marks it used, and installs it as a root
  Untyped capability (rights READ|WRITE) in the root cspace. This repeats
  until the reserve is reached.
- Conservation check: `sum(root Untyped frames) + pmm_free_count() ==`
  `frames that were usable at boot`.

The root cspace is handed to the first user task once threads exist
(ADR 0003, step 4).

## Known limitations

- *Retired by ADR 0010 (SC23, SC24):* dying CNodes are drained by the
  per-CPU reaper in bounded chunks, and the "zombie capabilities"
  anticipated below are replaced by dying objects, which need no slot.
- ~~CNode finalisers recurse.~~ Fixed (ADR 0008, T33): final CNodes go
  onto an intrusive pending list drained by a single loop, so deleting a
  chain of any depth uses constant kernel stack. Draining a huge tree
  still runs to completion in one kernel entry; making it preemptible
  (zombie capabilities, as in seL4) is part of the scheduler step.
- **Cycles:** a CNode holding a capability to itself is never finalised by
  ordinary deletes. It is still reclaimed when its Untyped is revoked
  (O9), so memory is never permanently lost.
- Revocation re-scans after any step whose finaliser deleted more than one
  capability, which makes revoking huge CNode trees O(n²) in the worst
  case. That's acceptable for now, and revocation is budgeted, so the
  kernel never stalls.
