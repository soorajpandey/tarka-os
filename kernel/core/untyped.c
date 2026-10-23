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
#include <tarka/endpoint.h>
#include <tarka/notification.h>
#include <tarka/thread.h>
#include <tarka/vspace.h>

_Static_assert(sizeof(struct cap) == (1u << CAP_SLOT_BITS), "a CNode slot is exactly one record");

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

void untyped_boot(void)
{
    paddr_t base;
    unsigned bits;
    uint32_t n = 0;

    cspace_init(&root_cs, root_slots, ROOT_CSPACE_SLOTS);
    /* A CNode has no finaliser: every CNode dies through the reaper,
     * one slot per unit (ADR 0010 SC23, SC24; this replaces the pending
     * list of ADR 0008 T33, which drained in one entry). */
    /* SC18: the frame table exists before any memory can become a Frame. */
    objmeta_boot();

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
    case CAP_NOTIFICATION:
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

    /* 5a. the derivation freeze (ADR 0010 SC22a): nothing is carved from
     * an Untyped that is frozen or lies under a frozen one */
    if (obj_frozen(ut))
        return T_EBUSY;

    /* 5b. serials (SC21, revision 3d D3): never reused, so creation fails
     * once they run out */
    if ((rc = serial_check(count)))
        return rc;

    /* 6. reset rule (O7, amended by ADR 0010 SC23): the memory is
     * reusable only when nothing carved from it is alive or dying */
    bool may_reset = !cap_has_children(ut);
    if (may_reset && ut->ut.dying == 0)
        ut->ut_watermark = 0;

    /* 7. placement (O5) */
    uint64_t obj_size = 1ull << obj_bits;
    uint64_t start = ALIGN_UP(ut->ut_watermark, obj_size);
    uint64_t span, end;
    if (mul_overflow((uint64_t)count, obj_size, &span) || add_overflow(start, span, &end) ||
        end > (1ull << ut_bits))
        /* A reset would make room once the reaper has finished: retry
         * later. A plain error; nothing is reserved or queued. */
        return may_reset && ut->ut.dying ? T_EAGAIN : T_ENOMEM;

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
        else if (type == CAP_NOTIFICATION)
            notification_obj_init(pa_add(base, i * obj_size));
        else if (type == CAP_ENDPOINT)
            endpoint_obj_init(pa_add(base, i * obj_size));
    }

    /* 9. capabilities, as children of the Untyped. Untyped and PageTable
     * capabilities are never copyable, so they get no GRANT. */
    cap_rights_t rights = (type == CAP_UNTYPED || type == CAP_PAGE_TABLE)
                              ? (CAP_R_READ | CAP_R_WRITE) : CAP_R_ALL;
    for (uint32_t i = 0; i < count; i++) {
        struct cap *c = &dst_cs->slots[dst + i];

        cap_slot_clear(c);
        c->type = type;
        c->rights = rights;
        c->object = cap_obj_encode(pa_add(base, i * obj_size), obj_bits);
        meta_new_object(c, ut, serial_take());   /* carver (SC22a), serial (SC21) */
        cap_insert_child(ut, c);
        meta_add(c);              /* C4 */
    }
    ut->ut_watermark = end;
    return T_OK;
}

int cnode_view_cap(const struct cap *c, struct cspace *out)
{
    if (c->type != CAP_CNODE)
        return T_EBADCAP;
    *out = cnode_of(cap_obj_paddr(c), cap_obj_bits(c));
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
