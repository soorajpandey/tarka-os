#ifndef TARKA_CAP_H
#define TARKA_CAP_H

#include <tarka/types.h>

/*
 * Capabilities are the only form of authority in Tarka. A task can act on
 * a kernel object only through a capability held in its capability space
 * (cspace). There are no ambient permissions: no root user, no global
 * object IDs that bypass the cspace. See docs/adr/0002-capabilities.md and
 * docs/adr/0003-kernel-object-model.md.
 */

enum cap_type {
    CAP_NULL = 0,   /* empty slot */
    CAP_UNTYPED,    /* raw physical memory that can be retyped */
    CAP_FRAME,      /* a mappable physical page */
    CAP_ENDPOINT,   /* IPC endpoint */
    CAP_IRQ,        /* right to receive one interrupt line */
    CAP_IOPORT,     /* range of x86 I/O ports */
    CAP_CNODE,      /* another capability table */
    CAP_THREAD,     /* thread control */
    CAP_ADDRESS_SPACE, /* PML4 + shadow (ADR 0007) */
    CAP_PAGE_TABLE, /* one page-table level + shadow (ADR 0007) */
    CAP_TYPE_COUNT,
};

typedef uint32_t cap_rights_t;

#define CAP_R_READ   (1u << 0)
#define CAP_R_WRITE  (1u << 1)
#define CAP_R_EXEC   (1u << 2)
#define CAP_R_GRANT  (1u << 3)  /* may be copied/derived to another slot */
#define CAP_R_ALL    (CAP_R_READ | CAP_R_WRITE | CAP_R_EXEC | CAP_R_GRANT)

/*
 * A capability slot. It lives only in kernel memory and contains kernel
 * pointers, so it must never be copied to user space as-is: user space
 * gets a struct cap_info from cap_identify() instead.
 *
 * Capabilities derived from one another form the capability derivation
 * tree (CDT), stored as a doubly linked list in pre-order with a depth per
 * node: the descendants of a node are the entries that follow it with a
 * greater depth. The tree spans every cspace, so revocation reaches copies
 * wherever they were delegated.
 */
struct cap {
    uint32_t     type;     /* enum cap_type */
    cap_rights_t rights;
    uint64_t     object;   /* kernel object reference (type-specific) */
    union {
        uint64_t badge;        /* CAP_ENDPOINT: caller-visible tag, set once */
        uint64_t ut_watermark; /* CAP_UNTYPED: bytes handed out (ADR 0006) */
        uint64_t map_state;    /* CAP_FRAME: va | rights | MAPPED;
                                * CAP_PAGE_TABLE: depth | INSTALLED (ADR 0007) */
    };
    struct cap  *prev;     /* CDT pre-order links */
    struct cap  *next;
    uint32_t     depth;    /* CDT depth; children are parent depth + 1 */
    uint32_t     slot_flags; /* property of the slot, not of the capability in
                              * it: 0 for ordinary slots; SLOT_BINDING | index
                              * for a thread's binding slots (ADR 0008, T2) */
    uint64_t    *map_entry; /* CAP_FRAME / CAP_PAGE_TABLE: the hardware entry
                             * this capability filled, or NULL */
    uint64_t     pending_link; /* property of the slot: zero, except in slot 0 of
                                * a CNode waiting to be drained (ADR 0008,
                                * T33), see cnode_finalise() */
};

_Static_assert(sizeof(struct cap) == 64, "struct cap must have no implicit padding");

#define SLOT_BINDING     0x80000000u
#define SLOT_BINDING_IDX 0x000000FFu

/* What user space may learn about a capability. Contains no kernel
 * pointers and no padding, so it is safe to copy out. */
struct cap_info {
    uint32_t     type;
    cap_rights_t rights;
    uint64_t     badge;
};

_Static_assert(sizeof(struct cap_info) == 16, "struct cap_info must have no padding");

/*
 * Retyped objects and Untypeds encode their physical range in `object` as
 * `paddr | size_bits`: every such object is at least 64 bytes and
 * naturally aligned, so the low 6 bits are free.
 */
#define CAP_OBJ_BITS_MASK 0x3Full

static inline uint64_t cap_obj_encode(paddr_t paddr, unsigned bits)
{
    return pa_raw(paddr) | bits;
}

static inline paddr_t cap_obj_paddr(const struct cap *c)
{
    return pa(c->object & ~CAP_OBJ_BITS_MASK);
}

static inline unsigned cap_obj_bits(const struct cap *c)
{
    return (unsigned)(c->object & CAP_OBJ_BITS_MASK);
}

struct cspace {
    struct cap *slots;
    uint32_t    nslots;
};

typedef uint32_t cap_slot_t;

/* Called when the last capability to an object is deleted, after the slot
 * has been cleared, so the owning subsystem can destroy the object. */
typedef void (*cap_finaliser_t)(enum cap_type type, uint64_t object);

void cap_set_finaliser(enum cap_type type, cap_finaliser_t fn);

/* Called for every deleted capability of `type` (not only the last one),
 * while it is still intact and linked, so per-capability state such as a
 * frame mapping can be undone (ADR 0007, V12). */
typedef void (*cap_cleanup_t)(struct cap *c);

void cap_set_cleanup(enum cap_type type, cap_cleanup_t fn);

/* Zero `storage` and use it as a cspace. The storage must not hold live
 * capabilities: other CDT nodes may link to them, and zeroing would leave
 * those links dangling. Use cspace_destroy() first. */
void cspace_init(struct cspace *cs, struct cap *storage, uint32_t nslots);

/* Delete every capability in `cs` so that no CDT link points into its
 * storage. Untypeds are revoked first (ADR 0006, O8). Must be called
 * before the storage is freed or goes out of scope. */
void cspace_destroy(struct cspace *cs);

/* Install a new root capability. Only the kernel calls this while
 * constructing the initial task; it is never reachable from user space. */
__must_check int cap_install(struct cspace *cs, cap_slot_t slot, enum cap_type type,
                             cap_rights_t rights, uint64_t object);

/* Validate that `slot` holds a capability of `type` carrying every right
 * in `need`. This is the single choke point for every syscall that takes
 * a capability argument. */
__must_check int cap_lookup(const struct cspace *cs, cap_slot_t slot, enum cap_type type,
                            cap_rights_t need, const struct cap **out);

/* Report a capability's type, rights and badge without exposing kernel
 * state. Receivers use this to check capabilities they were sent. */
__must_check int cap_identify(const struct cspace *cs, cap_slot_t slot, struct cap_info *out);

/* Derive a copy of `src` into the empty slot `dst`, possibly in another
 * cspace, with rights narrowed to `src.rights & mask`. Rights can never be
 * widened. Requires CAP_R_GRANT on the source. A non-zero badge may only be
 * set on an unbadged source, and only on endpoints. The copy becomes a CDT
 * child of the source and never inherits per-capability state (a copy of a
 * mapped frame starts unmapped). Untyped and PageTable capabilities cannot
 * be copied (ADR 0006 O2, ADR 0007 V10): T_EINVAL. */
__must_check int cap_derive_to(struct cspace *src_cs, cap_slot_t src,
                               struct cspace *dst_cs, cap_slot_t dst,
                               cap_rights_t mask, uint64_t badge);

static inline __must_check int cap_derive(struct cspace *cs, cap_slot_t src, cap_slot_t dst,
                                          cap_rights_t mask, uint64_t badge)
{
    return cap_derive_to(cs, src, cs, dst, mask, badge);
}

/* Delete one capability. Its CDT children move up to its parent. If it was
 * the last capability to its object, the type's finaliser runs. An Untyped
 * that still has children cannot be deleted (ADR 0006, O8): T_EBUSY. */
__must_check int cap_delete(struct cspace *cs, cap_slot_t slot);

/* Delete every capability derived from `slot` (in any cspace), keeping
 * `slot` itself. Deletes at most `budget` capabilities per call (0 means
 * no limit) and returns T_EAGAIN if more remain; calling again resumes
 * where it stopped, so a huge subtree never blocks the kernel. */
__must_check int cap_revoke(struct cspace *cs, cap_slot_t slot, uint32_t budget);

#endif
