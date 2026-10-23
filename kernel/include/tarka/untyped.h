#ifndef TARKA_UNTYPED_H
#define TARKA_UNTYPED_H

#include <tarka/cap.h>

/* Untyped memory and retype. The specification is
 * docs/adr/0006-untyped-retype.md; rule numbers (O1..O11) refer to it. */

#define UNTYPED_MIN_BITS    12
#define UNTYPED_MAX_BITS    47
#define UNTYPED_BOOT_MAX_BITS 30   /* largest root Untyped handed out at boot */
#define RETYPE_MAX_COUNT    256
#define CNODE_MAX_SLOT_BITS 12
#define FRAME_BITS          12
#define ENDPOINT_BITS       6
#define CAP_SLOT_BITS       7      /* a CNode slot occupies 128 bytes (ADR 0010, SC20a) */

/* Frames kept in the kernel's own allocator; everything else becomes
 * Untyped. */
#define KERNEL_RESERVE_FRAMES 2048 /* 8 MiB */

#define ROOT_CSPACE_SLOTS 256

/* Hand all usable memory except the kernel reserve to root Untyped
 * capabilities in the root cspace, and register object finalisers. */
void untyped_boot(void);

struct cspace *untyped_root_cspace(void);

/* Frames covered by root Untypeds handed out at boot. */
uint64_t untyped_boot_frames(void);

/*
 * Create `count` objects of `type` from the Untyped in `ut_slot`, placing
 * their capabilities in `dst_cs` slots dst..dst+count-1 (O3, O4). For
 * CAP_CNODE, `size_bits` is log2 of the slot count; for CAP_UNTYPED it is
 * log2 of the child size; for fixed-size types it must be 0.
 */
__must_check int cap_retype(struct cspace *cs, cap_slot_t ut_slot, enum cap_type type,
                            unsigned size_bits, struct cspace *dst_cs, cap_slot_t dst,
                            uint32_t count);

/* A fresh view of the CNode named by `slot`, valid only until the next
 * capability operation. Never store it (O10). */
__must_check int cnode_view(const struct cspace *cs, cap_slot_t slot, struct cspace *out);

/* The same, from a CNode capability held anywhere (e.g. a thread's
 * cspace binding). */
__must_check int cnode_view_cap(const struct cap *c, struct cspace *out);

#endif
