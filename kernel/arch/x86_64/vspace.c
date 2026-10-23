/*
 * Address spaces and page tables (x86-64, 4-level paging).
 * Specification: docs/adr/0007-address-spaces.md.
 *
 * AddressSpace and PageTable objects are two pages: the hardware table,
 * followed by a shadow page holding, per entry, the capability that filled
 * it. A capability that fills an entry records it in map_entry; the shadow
 * slot is always map_entry + 512.
 */

#include <tarka/vspace.h>

#include "../../core/cap_internal.h"
#include "cpu.h"
#include "paging.h"

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/string.h>
#include <tarka/thread.h>

#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ull
/* Accessed and Dirty are set by the MMU itself; the audit ignores them. */
#define PTE_HW_STATUS  ((1ull << 5) | (1ull << 6))
#define ENTRIES        512
#define USER_ENTRIES   256

/* map_state encoding */
#define MS_MAPPED      (1ull << 3)          /* CAP_FRAME */
#define MS_RIGHTS      (CAP_R_READ | CAP_R_WRITE | CAP_R_EXEC)
#define MS_INSTALLED   (1ull << 3)          /* CAP_PAGE_TABLE; low bits = depth */
#define MS_DEPTH       0x3ull

_Static_assert((PAGE_SIZE - 1) >= (MS_MAPPED | MS_RIGHTS), "map_state flags must fit below the page offset");

static paddr_t master_pml4;

static inline uint64_t *table_va(paddr_t p) { return phys_to_virt(p); }
static inline paddr_t cr3_pa(void) { return pa(read_cr3() & PTE_ADDR_MASK); }
static inline void load_cr3(paddr_t p) { write_cr3(pa_raw(p)); }
static inline paddr_t pte_pa(uint64_t e) { return pa(e & PTE_ADDR_MASK); }
static inline uint64_t pte_make(paddr_t p, uint64_t flags) { return pa_raw(p) | flags; }
static inline struct cap **shadow_of(uint64_t *entry) { return (struct cap **)(entry + ENTRIES); }

/* Phase 1 of V13: one CPU; drop every non-global translation. User
 * entries are never global. */
static void tlb_flush_user(void)
{
    write_cr3(read_cr3());
}

/* Forget that `c` fills a hardware entry (the entry itself is cleared by
 * the caller). */
static void cap_detach(struct cap *c)
{
    c->map_state = 0;
    c->map_entry = NULL;
}

static void entry_clear(uint64_t *entry)
{
    *entry = 0;
    *shadow_of(entry) = NULL;
}

/* Clear the first `nentries` entries of the table at `depth` (0 = PML4)
 * and, recursively, of every table below it, detaching each capability
 * that filled an entry (V12, V15). Depth is at most 3, so the recursion
 * is bounded. */
static void subtree_teardown(paddr_t table, unsigned depth, unsigned nentries)
{
    uint64_t *t = table_va(table);

    for (unsigned i = 0; i < nentries; i++) {
        struct cap *child = *shadow_of(&t[i]);
        if (!child)
            continue;
        if (depth < 3)
            subtree_teardown(cap_obj_paddr(child), depth + 1, ENTRIES);
        cap_detach(child);
        entry_clear(&t[i]);
    }
}

static void frame_cleanup(struct cap *c)
{
    if (c->map_state & MS_MAPPED) {
        entry_clear(c->map_entry);
        cap_detach(c);
        tlb_flush_user();
    }
}

/* V15: a table that is not installed holds no mappings, so removing it
 * from its parent also empties it. Otherwise a populated table could be
 * re-installed in another address space, exposing frames there without
 * anyone using a frame capability for that address space. */
static void pt_cleanup(struct cap *c)
{
    if (c->map_state & MS_INSTALLED) {
        unsigned depth = c->map_state & MS_DEPTH;

        entry_clear(c->map_entry);
        cap_detach(c);
        subtree_teardown(cap_obj_paddr(c), depth, ENTRIES);
        tlb_flush_user();
    }
}

/* PageTable capabilities cannot be copied, so pt_cleanup has always run
 * by now; if the table was never installed it holds nothing. */
static void pt_finalise(enum cap_type type, uint64_t object)
{
    (void)type;
    (void)object;
}

static void as_finalise(enum cap_type type, uint64_t object)
{
    paddr_t pml4 = pa(object & ~CAP_OBJ_BITS_MASK);

    (void)type;
    if (pa_raw(cr3_pa()) == pa_raw(pml4))
        vspace_activate_kernel();
    subtree_teardown(pml4, 0, USER_ENTRIES);
    tlb_flush_user();
}

void vspace_boot(void)
{
    master_pml4 = cr3_pa();

    /* V4: make the whole kernel half supervisor-only at the top level. The
     * bootloader sets U=1 on intermediate entries and relies on every leaf
     * having U=0; clearing U here means no lower-level entry can ever make
     * kernel memory user-accessible, whatever it contains. */
    uint64_t *master = table_va(master_pml4);
    for (unsigned i = USER_ENTRIES; i < ENTRIES; i++)
        master[i] &= ~PTE_U;
    load_cr3(master_pml4);

    cap_set_cleanup(CAP_FRAME, frame_cleanup);
    cap_set_cleanup(CAP_PAGE_TABLE, pt_cleanup);
    cap_set_finaliser(CAP_PAGE_TABLE, pt_finalise);
    cap_set_finaliser(CAP_ADDRESS_SPACE, as_finalise);
}

void vspace_as_init(paddr_t as)
{
    uint64_t *pml4 = table_va(as);
    const uint64_t *master = table_va(master_pml4);

    KASSERT(!pa_is_null(master_pml4)); /* vspace_boot() must run first */
    /* V4: the kernel half is copied once and never written again. */
    for (unsigned i = USER_ENTRIES; i < ENTRIES; i++)
        pml4[i] = master[i];
}

void vspace_activate_kernel(void)
{
    load_cr3(master_pml4);
}

void vspace_activate_cap(const struct cap *as)
{
    KASSERT(as->type == CAP_ADDRESS_SPACE);
    if (pa_raw(cr3_pa()) != pa_raw(cap_obj_paddr(as)))
        load_cr3(cap_obj_paddr(as));
}

int vspace_activate(struct cspace *cs, cap_slot_t as)
{
    const struct cap *a;
    int rc = cap_lookup(cs, as, CAP_ADDRESS_SPACE, 0, &a);

    if (rc)
        return rc;
    load_cr3(cap_obj_paddr(a));
    return T_OK;
}

static unsigned index_at(uaddr_t va, unsigned depth)
{
    return (ua_raw(va) >> (39 - 9 * depth)) & (ENTRIES - 1);
}

/* The entry at table `depth` (0 = PML4) covering `va`, if every table
 * above it is present; NULL otherwise. */
static uint64_t *entry_at(paddr_t pml4, uaddr_t va, unsigned depth)
{
    uint64_t *t = table_va(pml4);

    for (unsigned d = 0; d < depth; d++) {
        uint64_t e = t[index_at(va, d)];
        if (!(e & PTE_P))
            return NULL;
        t = table_va(pte_pa(e));
    }
    return &t[index_at(va, depth)];
}

/* Only R, RW and RX are valid mapping rights (V5). */
static bool rights_valid(cap_rights_t r)
{
    return r == CAP_R_READ || r == (CAP_R_READ | CAP_R_WRITE) ||
           r == (CAP_R_READ | CAP_R_EXEC);
}

static uint64_t pte_flags(cap_rights_t r)
{
    uint64_t f = PTE_P | PTE_U;

    if (r & CAP_R_WRITE)
        f |= PTE_W;
    if (!(r & CAP_R_EXEC))
        f |= PTE_NX;
    return f;
}

/* A capability bound as a thread's IPC buffer is a writable alias: the
 * kernel writes into its frame (ADR 0009, P5). */
static bool ipcbuf_binding(const struct cap *c)
{
    return c->slot_flags == (SLOT_BINDING | TS_IPCBUF);
}

/* V6: refuse a new alias that would make one physical frame writable
 * through one capability and executable through another. `skip` is the
 * capability that will hold the new alias, if it is one of the frame's
 * existing records (a mapping); NULL when the alias is a new record (a
 * binding), so that every existing record is checked, the source's own
 * mapping included. */
static bool alias_conflict(struct cap *frame, const struct cap *skip, cap_rights_t want)
{
    for (struct cap *c = cap_object_first(frame); c; c = cap_object_next(c)) {
        if (c == skip)
            continue;
        bool w = ((c->map_state & MS_MAPPED) && (c->map_state & CAP_R_WRITE)) || ipcbuf_binding(c);
        bool x = (c->map_state & MS_MAPPED) && (c->map_state & CAP_R_EXEC);
        if ((want & CAP_R_WRITE) && x)
            return true;
        if ((want & CAP_R_EXEC) && w)
            return true;
    }
    return false;
}

bool vspace_frame_alias_conflict(struct cap *frame, cap_rights_t want)
{
    return alias_conflict(frame, NULL, want);
}

int vspace_install(struct cspace *cs, cap_slot_t as, cap_slot_t pt, uaddr_t va, unsigned depth)
{
    const struct cap *a, *p;
    int rc;

    if ((rc = cap_lookup(cs, as, CAP_ADDRESS_SPACE, CAP_R_WRITE, &a)))
        return rc;
    if ((rc = cap_lookup(cs, pt, CAP_PAGE_TABLE, CAP_R_WRITE, &p)))
        return rc;
    if (depth < 1 || depth > 3)
        return T_EINVAL;

    /* The table at `depth` covers 2^(48 - 9*depth) bytes. */
    uint64_t span = 1ull << (48 - 9 * depth);
    if (ua_raw(va) >= USER_TOP || ua_raw(va) % span)
        return T_EINVAL; /* V3: never in the kernel half */

    struct cap *ptc = cap_slot(cs, pt);
    if (ptc->map_state & MS_INSTALLED)
        return T_EBUSY;

    uint64_t *entry = entry_at(cap_obj_paddr(a), va, depth - 1);
    if (!entry)
        return T_ENOENT;
    if (*entry & PTE_P)
        return T_EBUSY;

    /* Intermediate entries are permissive; leaves decide R/W/X. */
    *entry = pte_make(cap_obj_paddr(ptc), PTE_P | PTE_W | PTE_U);
    *shadow_of(entry) = ptc;
    ptc->map_state = depth | MS_INSTALLED;
    ptc->map_entry = entry;
    return T_OK;
}

int vspace_uninstall(struct cspace *cs, cap_slot_t pt)
{
    const struct cap *p;
    int rc = cap_lookup(cs, pt, CAP_PAGE_TABLE, 0, &p);

    if (rc)
        return rc;
    struct cap *ptc = cap_slot(cs, pt);
    if (!(ptc->map_state & MS_INSTALLED))
        return T_EINVAL;
    pt_cleanup(ptc);
    return T_OK;
}

int vspace_map(struct cspace *cs, cap_slot_t as, cap_slot_t frame, uaddr_t va,
               cap_rights_t rights)
{
    const struct cap *a, *f;
    int rc;

    if ((rc = cap_lookup(cs, as, CAP_ADDRESS_SPACE, CAP_R_WRITE, &a)))
        return rc;
    if ((rc = cap_lookup(cs, frame, CAP_FRAME, 0, &f)))
        return rc;
    if (!rights_valid(rights))
        return T_EINVAL; /* V5: no RWX, no WX, no empty mapping */
    if (ua_raw(va) >= USER_TOP || ua_raw(va) % PAGE_SIZE)
        return T_EINVAL; /* V1/V3: user half only */
    if ((f->rights & rights) != rights)
        return T_EPERM;  /* V9: cannot grant more than the frame cap holds */

    struct cap *fc = cap_slot(cs, frame);
    if (fc->map_state & MS_MAPPED)
        return T_EBUSY;  /* V10: one mapping per capability */
    if (alias_conflict(fc, fc, rights))
        return T_EPERM;  /* V6 */

    uint64_t *entry = entry_at(cap_obj_paddr(a), va, 3);
    if (!entry)
        return T_ENOENT; /* the kernel never allocates a table for you */
    if (*entry & PTE_P)
        return T_EBUSY;

    *entry = pte_make(cap_obj_paddr(fc), pte_flags(rights));
    *shadow_of(entry) = fc;
    fc->map_state = ua_raw(va) | rights | MS_MAPPED;
    fc->map_entry = entry;
    return T_OK;
}

int vspace_unmap(struct cspace *cs, cap_slot_t frame)
{
    const struct cap *f;
    int rc = cap_lookup(cs, frame, CAP_FRAME, 0, &f);

    if (rc)
        return rc;
    struct cap *fc = cap_slot(cs, frame);
    if (!(fc->map_state & MS_MAPPED))
        return T_EINVAL;
    frame_cleanup(fc);
    return T_OK;
}

int vspace_protect(struct cspace *cs, cap_slot_t frame, cap_rights_t rights)
{
    const struct cap *f;
    int rc = cap_lookup(cs, frame, CAP_FRAME, 0, &f);

    if (rc)
        return rc;
    struct cap *fc = cap_slot(cs, frame);
    if (!(fc->map_state & MS_MAPPED))
        return T_EINVAL;

    cap_rights_t cur = fc->map_state & MS_RIGHTS;
    /* V7: rights can only be removed in place. */
    if (!rights_valid(rights) || (rights & ~cur))
        return T_EPERM;

    *fc->map_entry = pte_make(cap_obj_paddr(fc), pte_flags(rights));
    fc->map_state = (fc->map_state & ~(uint64_t)MS_RIGHTS) | rights;
    tlb_flush_user();
    return T_OK;
}

/* ---- audit (tests) ---- */

static bool audit_table(paddr_t table, unsigned depth, unsigned nentries)
{
    uint64_t *t = table_va(table);

    for (unsigned i = 0; i < nentries; i++) {
        uint64_t e = t[i] & ~PTE_HW_STATUS;
        struct cap *c = *shadow_of(&t[i]);

        if (!(e & PTE_P)) {
            if (e || c)
                return false; /* stale entry or stale shadow */
            continue;
        }
        if (!c || c->map_entry != &t[i] || pa_raw(pte_pa(e)) != pa_raw(cap_obj_paddr(c)))
            return false;
        if (!(e & PTE_U))
            return false;

        if (depth < 3) {
            /* Intermediate entry: must be an installed PageTable of the
             * next depth (V3). */
            if (c->type != CAP_PAGE_TABLE || c->map_state != ((depth + 1) | MS_INSTALLED))
                return false;
            if (!audit_table(cap_obj_paddr(c), depth + 1, ENTRIES))
                return false;
        } else {
            /* Leaf: must be a mapped Frame whose rights match the entry
             * (V1, V5). */
            cap_rights_t r = c->map_state & MS_RIGHTS;
            if (c->type != CAP_FRAME || !(c->map_state & MS_MAPPED) || !rights_valid(r))
                return false;
            if ((e & ~PTE_ADDR_MASK) != pte_flags(r))
                return false;
            if ((e & PTE_W) && !(e & PTE_NX))
                return false;
        }
    }
    return true;
}

bool vspace_audit(const struct cspace *cs, cap_slot_t as)
{
    const struct cap *a;

    if (cap_lookup(cs, as, CAP_ADDRESS_SPACE, 0, &a))
        return false;

    const uint64_t *pml4 = table_va(cap_obj_paddr(a));
    const uint64_t *master = table_va(master_pml4);
    for (unsigned i = USER_ENTRIES; i < ENTRIES; i++) {
        if ((pml4[i] ^ master[i]) & ~PTE_HW_STATUS || (pml4[i] & PTE_U))
            return false; /* V4 */
        if (*shadow_of((uint64_t *)&pml4[i]))
            return false;
    }
    return audit_table(cap_obj_paddr(a), 0, USER_ENTRIES);
}
