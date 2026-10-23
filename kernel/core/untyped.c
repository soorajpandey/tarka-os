/*
 * Untyped memory and retype: the only way kernel objects come into
 * existence after boot. Specification: docs/adr/0006-untyped-retype.md.
 */

#include <tarka/untyped.h>

#include "cap_internal.h"

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/pmm.h>
#include <tarka/string.h>
#include <tarka/thread.h>
#include <tarka/vspace.h>

_Static_assert(sizeof(struct cap) <= (1u << CAP_SLOT_BITS), "CNode slot size too small");

static struct cap root_slots[ROOT_CSPACE_SLOTS];
static struct cspace root_cs;
static uint64_t boot_frames;

struct cspace *untyped_root_cspace(void) { return &root_cs; }
uint64_t untyped_boot_frames(void) { return boot_frames; }

static struct cspace cnode_of(paddr_t paddr, unsigned obj_bits)
{
    return (struct cspace){
        .slots = phys_to_virt(paddr),
        .nslots = 1u << (obj_bits - CAP_SLOT_BITS),
    };
}

/*
 * Last capability to a CNode deleted: delete everything stored in it so
 * no capability (and no CDT link) survives in memory that is about to
 * become reusable (O10).
 *
 * Without recursion (ADR 0008, T33): deleting a CNode's contents can make
 * other CNodes final, to any depth a user cares to build. Instead of
 * recursing, a final CNode is pushed onto a pending list and a single loop
 * drains the list, so kernel stack use is the same for one CNode or a
 * chain of a million. The list is intrusive: the link lives in slot 0's
 * `pending_link`, tagged with the CNode's size in its low 6 bits (slots
 * are 64-byte aligned). Clearing a slot preserves that word, so a
 * revocation that deletes the capability in a pending CNode's slot 0
 * cannot cut the list.
 */
static uint64_t pending_cnodes;   /* tagged pointer to slot 0, or 0 */
static bool draining;

static void cnode_finalise(enum cap_type type, uint64_t object)
{
    (void)type;
    struct cspace cn = cnode_of(pa(object & ~CAP_OBJ_BITS_MASK),
                                (unsigned)(object & CAP_OBJ_BITS_MASK));

    KASSERT(cn.slots[0].pending_link == 0);
    cn.slots[0].pending_link = pending_cnodes;
    pending_cnodes = (uint64_t)(uintptr_t)cn.slots | (object & CAP_OBJ_BITS_MASK);
    if (draining)
        return; /* the loop below, further up the stack, will get to it */

    draining = true;
    while (pending_cnodes) {
        struct cap *slots = (struct cap *)(uintptr_t)(pending_cnodes & ~CAP_OBJ_BITS_MASK);
        unsigned bits = (unsigned)(pending_cnodes & CAP_OBJ_BITS_MASK);

        pending_cnodes = slots[0].pending_link;   /* pop before emptying */
        slots[0].pending_link = 0;

        struct cspace view = { .slots = slots, .nslots = 1u << (bits - CAP_SLOT_BITS) };
        cspace_destroy(&view);                     /* may push more */
    }
    draining = false;
}

void untyped_boot(void)
{
    paddr_t base;
    unsigned bits;
    uint32_t n = 0;

    cspace_init(&root_cs, root_slots, ROOT_CSPACE_SLOTS);
    cap_set_finaliser(CAP_CNODE, cnode_finalise);

    while (n < ROOT_CSPACE_SLOTS &&
           pmm_take_block(UNTYPED_BOOT_MAX_BITS, KERNEL_RESERVE_FRAMES, &base, &bits)) {
        int rc = cap_install(&root_cs, n++, CAP_UNTYPED, CAP_R_READ | CAP_R_WRITE,
                             cap_obj_encode(base, bits));
        KASSERT(rc == T_OK);
        boot_frames += 1ull << (bits - 12);
    }

    kprintf("untyped: %u root Untypeds, %lu MiB; kernel keeps %lu MiB\n", n,
            boot_frames * PAGE_SIZE >> 20, pmm_free_count() * PAGE_SIZE >> 20);
}

/* O4 step 3: object size for a retype request, or 0 if invalid. */
static unsigned object_bits(enum cap_type type, unsigned size_bits, unsigned ut_bits)
{
    switch (type) {
    case CAP_FRAME:
        return size_bits == 0 ? FRAME_BITS : 0;
    case CAP_ENDPOINT:
        return size_bits == 0 ? ENDPOINT_BITS : 0;
    case CAP_CNODE:
        if (size_bits < 1 || size_bits > CNODE_MAX_SLOT_BITS)
            return 0;
        return size_bits + CAP_SLOT_BITS;
    case CAP_ADDRESS_SPACE:
    case CAP_PAGE_TABLE:
        return size_bits == 0 ? VSPACE_OBJ_BITS : 0;
    case CAP_THREAD:
        return size_bits == 0 ? THREAD_OBJ_BITS : 0;
    case CAP_UNTYPED:
        /* Strictly smaller: a child must never denote the same range as
         * its parent. */
        if (size_bits < UNTYPED_MIN_BITS || size_bits >= ut_bits)
            return 0;
        return size_bits;
    default:
        return 0; /* not retypeable (yet) */
    }
}

int cap_retype(struct cspace *cs, cap_slot_t ut_slot, enum cap_type type,
               unsigned size_bits, struct cspace *dst_cs, cap_slot_t dst, uint32_t count)
{
    const struct cap *cut;
    int rc;

    /* 1. authority */
    rc = cap_lookup(cs, ut_slot, CAP_UNTYPED, CAP_R_WRITE, &cut);
    if (rc)
        return rc;
    struct cap *ut = cap_slot(cs, ut_slot);
    unsigned ut_bits = cap_obj_bits(ut);

    /* 2-3. type and size */
    unsigned obj_bits = object_bits(type, size_bits, ut_bits);
    if (!obj_bits || obj_bits > ut_bits)
        return T_EINVAL;

    /* 4. count */
    if (count < 1 || count > RETYPE_MAX_COUNT)
        return T_EINVAL;

    /* 5. destination slots exist and are empty */
    uint32_t dst_end;
    if (add_overflow(dst, count, &dst_end) || dst_end > dst_cs->nslots)
        return T_ERANGE;
    for (uint32_t i = dst; i < dst_end; i++) {
        if (dst_cs->slots[i].type != CAP_NULL)
            return T_EBUSY;
    }

    /* 6. reset rule (O7) */
    if (!cap_has_children(ut))
        ut->ut_watermark = 0;

    /* 7. placement (O5) */
    uint64_t obj_size = 1ull << obj_bits;
    uint64_t start = ALIGN_UP(ut->ut_watermark, obj_size);
    uint64_t span, end;
    if (mul_overflow((uint64_t)count, obj_size, &span) || add_overflow(start, span, &end) ||
        end > (1ull << ut_bits))
        return T_ENOMEM;

    /* No state has changed so far. From here on the operation cannot fail. */
    paddr_t base = pa_add(cap_obj_paddr(ut), start);

    /* 8. zero (O6); child Untypeds hold no data */
    if (type != CAP_UNTYPED)
        memset(phys_to_virt(base), 0, span);

    /* Type-specific initialisation of the zeroed objects. */
    for (uint32_t i = 0; i < count; i++) {
        if (type == CAP_ADDRESS_SPACE)
            vspace_as_init(pa_add(base, i * obj_size));
        else if (type == CAP_THREAD)
            thread_obj_init(pa_add(base, i * obj_size));
    }

    /* 9. capabilities, as children of the Untyped. Untyped and PageTable
     * capabilities are never copyable, so they get no GRANT. */
    cap_rights_t rights = (type == CAP_UNTYPED || type == CAP_PAGE_TABLE)
                              ? (CAP_R_READ | CAP_R_WRITE) : CAP_R_ALL;
    for (uint32_t i = 0; i < count; i++) {
        struct cap *c = &dst_cs->slots[dst + i];

        KASSERT(c->pending_link == 0);
        cap_slot_clear(c);
        c->type = type;
        c->rights = rights;
        c->object = cap_obj_encode(pa_add(base, i * obj_size), obj_bits);
        cap_insert_child(ut, c);
    }
    ut->ut_watermark = end;
    return T_OK;
}

int cnode_view(const struct cspace *cs, cap_slot_t slot, struct cspace *out)
{
    const struct cap *c;
    int rc = cap_lookup(cs, slot, CAP_CNODE, 0, &c);

    if (rc)
        return rc;
    *out = cnode_of(cap_obj_paddr(c), cap_obj_bits(c));
    return T_OK;
}
