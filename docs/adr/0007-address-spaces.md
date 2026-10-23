# ADR 0007: Address spaces, page tables, mapping and unmapping

Status: accepted

Refines ADR 0003, step 3. This is the specification; the code is
`kernel/arch/x86_64/vspace.c` and the tests are the `vspace_*` groups in
`kernel/core/selftest.c`. Rule numbers (V1..V15) are referenced from
`docs/SECURITY.md`.

## Objects

| Object | Size | Contents |
|---|---|---|
| `AddressSpace` | 8 KiB from Untyped | page 0: the hardware PML4; page 1: the **shadow** page |
| `PageTable` | 8 KiB from Untyped | page 0: one hardware table (PDPT, PD or PT); page 1: the shadow page |
| `Frame` | 4 KiB from Untyped (ADR 0006) | user data |

The **shadow** page holds, for each of the 512 hardware entries, a pointer
to the capability that put the entry there: the child `PageTable`
capability, or the `Frame` capability for a leaf. The capability in turn
records where it is installed. Every mapping is therefore findable from
both ends, which is what makes unmapping and teardown exact (V9–V11).

Like everything else, these objects are created only by `retype` from an
Untyped the caller holds (O3). **The kernel never allocates a page table
on a task's behalf.** If an intermediate table is missing, `map` fails
with `T_ENOENT` and the caller must retype and install one.

## The two halves

```text
0x0000_0000_0000_0000 ┐
                      │ user half: PML4 entries 0..255
0x0000_7FFF_FFFF_FFFF ┘ only these entries are ever written by map/install
        (non-canonical hole)
0xFFFF_8000_0000_0000 ┐
                      │ kernel half: PML4 entries 256..511
0xFFFF_FFFF_FFFF_FFFF ┘ copied from the kernel's master PML4 when the
                        AddressSpace is created; supervisor-only; never
                        written afterwards
```

## Rules

### Kernel memory is never user-mappable (U2)

**V1. User leaf entries only ever point to Frame objects.** A leaf entry
in the user half is written only by `vspace_map`, which takes the physical
address from a `CAP_FRAME` capability and from nothing else.

**V2. Frames never contain kernel memory.** Frames are created only by
retype from Untyped memory (O3). Untyped covers only usable RAM handed
out at boot, never the kernel image, the kernel's own allocator reserve,
bootloader, ACPI or MMIO ranges (O1). Within an Untyped, a Frame never
overlaps a kernel object such as a CNode, Endpoint, AddressSpace or
PageTable (O5), and memory is reused only after every object in it is
gone (O7).

**V3. Intermediate user entries only ever point to PageTable objects,**
written only by `vspace_install`, which rejects any address outside the
user half. So no path exists from a user-reachable entry to a kernel
table.

**V4. The kernel half is immutable and supervisor-only.** It is copied
from the master PML4 at creation and never written again: install and map
reject kernel addresses, and the kernel never adds new top-level kernel
entries after boot. The audit test checks that every AddressSpace's
kernel half matches the master exactly and has U=0.

Together V1–V4 make U2 *structural*: there is no combination of
capabilities that yields a user mapping of kernel memory, because no
operation ever writes anything but a Frame's address into a user leaf, or
a PageTable's address into a user intermediate entry.

### W^X (U3)

**V5. No single mapping is both writable and executable.** `map` accepts
exactly R, RW or RX. RWX, WX and "no rights" are `T_EINVAL`. Read-only and
writable mappings always carry NX.

**V6. No physical frame is ever writable through one mapping and
executable through another, in any address space.** Before mapping
writable, the kernel checks that no other capability to the same frame is
mapped executable, and vice versa (`T_EPERM`). *Amended by ADR 0010
(SC18, stage 2):* the check reads the frame's writable and executable
alias counts in the frame metadata table, which are updated in the same
step as every mapping, protection change, unmapping and IPC-buffer
binding; it no longer walks the frame's capabilities, and no longer
relies on them being contiguous in the CDT. When DMA domains arrive (ADR
0001), a device mapping counts as writable.

**V7. In-place permission changes can only remove rights.**
`vspace_protect` accepts any non-empty subset of the current rights (RW→R,
RX→R), and refuses RW→RX, RX→RW and R→anything (`T_EPERM`).

**V8. Writing and later executing a frame is a matter of authority.**
Unmapping RW and then mapping RX is a fresh `map`, which needs a Frame
capability holding both WRITE and EXEC (and passes V6 only once the
writable mapping is gone). The system loader holds such capabilities;
applications receive frames whose capabilities never combine WRITE and
EXEC, so for them write-then-execute is impossible, not merely
discouraged.

### Mapping authority (confused deputies)

**V9. Mapping needs all three authorities, each from the caller's own
cspace:**
- the AddressSpace capability with WRITE
- the Frame capability with every right being granted (READ for R, plus
  WRITE for W, plus EXEC for X)
- (for install) the PageTable capability with WRITE

A task cannot name another task's capability: slots are always resolved
in the caller's cspace. So "B convinces the kernel to map A's frame"
requires B to actually hold a capability to that frame, delegated by A.

**V10. One mapping per Frame capability.** A Frame capability that is
already mapped cannot be mapped again (`T_EBUSY`). To map a frame twice,
derive a second capability; a copy always starts unmapped. PageTable
capabilities cannot be copied at all, like Untyped: a table is installed
in at most one place.

### Unmapping and teardown

**V11. Who can remove a mapping:**
- the holder of the Frame capability that made it (`vspace_unmap`, or by
  deleting that capability)
- anyone who revokes an ancestor of that capability
- whoever deletes the PageTable or AddressSpace that contains it, which
  needs that object's last capability

Nobody else. Deleting a *copy* of a frame capability removes only that
copy's mapping.

**V12. Teardown leaves no dangling references** (O10):
- Deleting a mapped Frame capability first unmaps it.
- Deleting a PageTable first removes it from its parent, then clears every
  entry in its subtree and marks each child capability (frames and lower
  tables) as no longer mapped or installed (V15).
- Deleting an AddressSpace does the same for its user half, and if it is
  the active address space, switches the CPU to the kernel's master PML4
  first.

  *Amended by ADR 0010 revision 3d (D1), implemented at its stage 3:*
  deleting a PageTable removes it from its parent and clears **its own**
  entries only (at most 512), marking each direct child capability as no
  longer mapped or installed. Deleting an AddressSpace does the same for
  its 256 user entries. Lower tables are not walked: they become inert
  (V15') and keep their contents.

After any deletion, no hardware entry and no shadow pointer refers to a
deleted capability, and no capability refers to a deleted table.

**V15. A table that is not installed holds no mappings.** Removing a
PageTable from its parent (`vspace_uninstall`, or deleting the parent
table or AddressSpace) tears down its entire subtree: every lower table is
uninstalled and every frame below it unmapped. Otherwise whoever holds the
table could install it into a different address space and expose every
frame in it there, without anyone using a frame capability for that
address space, bypassing V9. The subtree is at most three levels deep, so
teardown is bounded.

*Superseded by V15' (ADR 0010 revision 3d, D1; implemented at its stage
3).* "At most three levels deep" bounds the depth of that teardown, not
its width: one uninstall could walk every table below it, so the work
of one entry was chosen by the caller. The replacement keeps V15's
purpose without walking:

**V15'. A populated table that is not installed is inert and cannot be
installed.**
- Uninstalling a table, or deleting its parent, clears one entry and
  flushes the TLB. **Detaching is not destroying the subtree:** the
  table keeps its entries, every lower table stays installed in it and
  every frame stays mapped, all owned by the same capabilities as
  before, whose holders can still unmap, uninstall and delete them.
  Nothing is reachable through it: no address-space root leads to it.
- Each table keeps a count of its filled entries. `vspace_install()`
  refuses a table whose count is not 0 with `T_EBUSY`. It reads the
  count itself, in the same step that makes the table reachable.
- So nobody can expose a table's frames in another address space by
  moving the table, because a populated table never moves.

### TLB invalidation

**V13. When unmap, protect, uninstall or teardown returns, no CPU can
still use the removed or weakened translation.** This is invariant S1 for
address spaces.
- Phase 1 (single CPU, now): every such operation flushes all non-global
  TLB entries on the local CPU, by reloading CR3. User entries are never
  global. Simple and obviously correct; precise `invlpg` and PCID come
  later as optimisations.
- Phase 2 (SMP, ADR 0005): each AddressSpace records which CPUs have run it
  since its last flush, in the unused kernel-half area of its shadow page.
  The operation sends shootdown IPIs to those CPUs and does not return
  until all have acknowledged. A Frame's memory is not reusable (O7)
  until the shootdown completes.

**V14. Frames are zeroed at creation (O6), not at unmap.** Unmapping
never exposes data; reuse of the memory requires the frame object to be
destroyed and its Untyped reset, after which any new object is zeroed.

## Operations

| Operation | Needs | Errors |
|---|---|---|
| `vspace_install(cs, as, pt, va, depth)` | AS: WRITE; PT: WRITE, not installed | `T_EINVAL`: va not user, not aligned to the region the table covers, or depth not 1..3. `T_ENOENT`: higher table missing. `T_EBUSY`: entry used, or PT already installed |
| `vspace_map(cs, as, frame, va, rights)` | AS: WRITE; Frame: rights ⊇ requested, not mapped | `T_EINVAL`: va not user/aligned, rights not R/RW/RX. `T_ENOENT`: table missing. `T_EBUSY`: entry used or cap mapped. `T_EPERM`: rights or alias rule (V6) |
| `vspace_unmap(cs, frame)` | Frame capability, mapped | `T_EINVAL` if not mapped |
| `vspace_uninstall(cs, pt)` | PageTable capability, installed | `T_EINVAL` if not installed |
| `vspace_protect(cs, frame, rights)` | Frame capability, mapped | `T_EPERM` unless rights are a non-empty subset of the current ones |
| `vspace_activate(cs, as)` | AS capability | Loads the AS into CR3 (used by threads later; by tests now) |

Depth is the level of the table being installed: 1 = PDPT (entry in the
PML4), 2 = PD, 3 = PT. Leaves are always 4 KiB pages in a depth-3 table.

## Tests required before step 3 is done

- Every permission combination: R, RW, RX, none, RWX, WX.
- Transitions: RW→R and RX→R allowed; RW→RX, RX→RW and R→RW refused.
- Aliasing, in both orders and across two address spaces: an RX mapping
  blocks any RW mapping of the same frame, and vice versa. RO aliases are
  allowed alongside either.
- Confused deputies:
  - mapping without the frame capability
  - with a capability lacking WRITE or EXEC
  - with a read-only AS capability
  - mapping a non-Frame capability (CNode, Endpoint, AS, PageTable)
  - mapping at kernel or non-canonical addresses
  - installing at kernel PML4 indices
  - mapping one capability twice
  - copying a PageTable capability
- Hardware truth: activate the address space and check the real page-table
  walk. Read user memory through the user address with SMAP explicitly
  lifted (`stac`/`clac`), to prove it maps the intended frame.
- Teardown in every order: frame, table or address space first; revoking
  the Untyped; deleting the active address space. Afterwards a structural
  audit finds no stale entry, shadow pointer or install record.
- An audit of every AddressSpace: kernel half identical to the master and
  supervisor-only, and every user leaf a Frame with U=1 and never W without NX.
