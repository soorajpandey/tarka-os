/*
 * Per-object metadata: live-record and rights counts, frame alias counts,
 * carving back-pointers and the derivation freeze flag (ADR 0010,
 * SC17-SC19a, SC22a).
 *
 * Where each object's metadata lives (SC19a):
 *   Endpoint, Notification, Thread   in the object
 *   CNode                            slot 0's slot_meta
 *   Frame, AddressSpace, PageTable   the frame table entry of its base page
 *   Untyped                          its own (unique) record's ut_meta
 *   test token (test builds)         a small table keyed by object value
 *   debug console                    none: no object, no finaliser
 *
 * Counts change only in commit steps (C4, C5): meta_add() right after a
 * record is linked in, meta_del() right before one is unlinked. Neither
 * can fail; the checks that could refuse (overflow, freeze) are in
 * meta_check_new(), which the derivation check calls.
 */

#include "cap_internal.h"

#include <tarka/endpoint.h>
#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/notification.h>
#include <tarka/pmm.h>
#include <tarka/string.h>
#include <tarka/thread.h>
#include <tarka/untyped.h>

/* The frame metadata table (SC18): one entry per physical page that the
 * page allocator covers, so every page that can ever back a Frame,
 * AddressSpace or PageTable has its entry from boot. Allocated once,
 * never freed, never grown. */
static struct obj_meta *frame_table;
static uint64_t frame_table_n;

void objmeta_boot(void)
{
    uint64_t n = pmm_frame_limit();
    uint64_t bytes, pages;

    KASSERT(frame_table == NULL);
    if (mul_overflow(n, (uint64_t)sizeof(struct obj_meta), &bytes))
        panic("objmeta: frame table size overflows");
    pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    paddr_t p = pmm_alloc_contig(pages);   /* zeroed */
    if (pa_is_null(p))
        panic("objmeta: no room for a %lu-page frame table", pages);
    frame_table = phys_to_virt(p);
    frame_table_n = n;
    kprintf("objmeta: frame table %lu KiB for %lu pages\n", pages * PAGE_SIZE >> 10, n);
}

static struct obj_meta *frame_meta(paddr_t base)
{
    uint64_t i = pa_raw(base) / PAGE_SIZE;

    KASSERT(frame_table != NULL && i < frame_table_n);
    return &frame_table[i];
}

#ifdef TARKA_SELFTEST
/* Test tokens have no object memory: their metadata is kept here, keyed
 * by the object value. An entry is in use while its count is non-zero. */
#define TOKEN_META_MAX 64
static struct { uint64_t object; struct obj_meta m; } token_meta[TOKEN_META_MAX];

static struct obj_meta *token_lookup(uint64_t object, bool create)
{
    for (unsigned i = 0; i < TOKEN_META_MAX; i++)
        if (token_meta[i].m.live && token_meta[i].object == object)
            return &token_meta[i].m;
    if (!create)
        return NULL;
    for (unsigned i = 0; i < TOKEN_META_MAX; i++)
        if (!token_meta[i].m.live) {
            memset(&token_meta[i], 0, sizeof(token_meta[i]));
            token_meta[i].object = object;
            return &token_meta[i].m;
        }
    panic("objmeta: test token table full");
}

bool objmeta_token_entry(unsigned i, uint64_t *object, const struct obj_meta **m)
{
    if (i >= TOKEN_META_MAX)
        return false;
    *object = token_meta[i].object;
    *m = &token_meta[i].m;
    return true;
}

bool objmeta_frame_entry(uint64_t i, const struct obj_meta **m)
{
    if (i >= frame_table_n)
        return false;
    *m = &frame_table[i];
    return true;
}
#endif

static struct obj_meta *lookup(const struct cap *c, bool create)
{
    (void)create;
    switch (c->type) {
    case CAP_UNTYPED:
        return (struct obj_meta *)&c->ut_meta;
    case CAP_FRAME:
    case CAP_ADDRESS_SPACE:
    case CAP_PAGE_TABLE:
        return frame_meta(cap_obj_paddr(c));
    case CAP_ENDPOINT:
        return &endpoint_of(c)->meta;
    case CAP_NOTIFICATION:
        return &notification_of(c)->meta;
    case CAP_THREAD:
        return &thread_of(c)->meta;
    case CAP_CNODE:
        return &((struct cap *)phys_to_virt(cap_obj_paddr(c)))[0].slot_meta;
#ifdef TARKA_SELFTEST
    case CAP_TEST_TOKEN:
        return token_lookup(c->object, create);
#endif
    default:
        return NULL;
    }
}

struct obj_meta *obj_meta(const struct cap *c)
{
    return lookup(c, false);
}

static bool counts_rights(uint32_t type)
{
    return type == CAP_ENDPOINT || type == CAP_NOTIFICATION;
}

/* A capability bound as a thread's IPC buffer: a writable alias of its
 * frame, because the kernel writes into it (ADR 0009, P5). */
bool cap_is_ipcbuf_binding(const struct cap *c)
{
    return c->type == CAP_FRAME && c->slot_flags == (SLOT_BINDING | TS_IPCBUF);
}

bool obj_frozen(const struct cap *c)
{
    const struct obj_meta *m = obj_meta(c);

    /* F2: the object's own flag, then each Untyped on its carving chain.
     * Each link is strictly smaller than the one above it, so there are
     * at most CHAIN_MAX of them. */
    for (unsigned links = 0; m; links++) {
        if (m->flags & OBJ_FROZEN)
            return true;
        if (!m->carver)
            break;
        KASSERT(links < CHAIN_MAX && m->carver->type == CAP_UNTYPED);
        m = &m->carver->ut_meta;
    }
    return false;
}

int meta_check_new(const struct cap *s)
{
    const struct obj_meta *m = obj_meta(s);

    if (obj_frozen(s))
        return T_EBUSY;     /* SC22a: no record enters a frozen subtree */
    if (m && m->live > OBJ_LIVE_MAX)
        return T_ERANGE;    /* SC18: refused, never wrapped */
    return T_OK;
}

void meta_new_object(struct cap *c, struct cap *carver)
{
    struct obj_meta *m = obj_meta(c);

    /* Retype's memory is fresh (O6) and frame table entries are cleared
     * when their object's last record goes, so nothing stale is here. */
    KASSERT(m != NULL);
    KASSERT(m->live == 0 && m->flags == 0 && m->n_read == 0 && m->n_write == 0 &&
            m->carver == NULL);
    KASSERT(carver == NULL || carver->type == CAP_UNTYPED);
    m->carver = carver;
}

void meta_add(struct cap *c)
{
    struct obj_meta *m = lookup(c, true);

    if (!m)
        return;
    KASSERT(m->live < UINT32_MAX);
    m->live++;
    if (counts_rights(c->type)) {
        if (c->rights & CAP_R_READ)
            m->n_read++;
        if (c->rights & CAP_R_WRITE)
            m->n_write++;
    }
    if (cap_is_ipcbuf_binding(c))
        m->alias_w++;
    KASSERT(m->n_read <= m->live && m->n_write <= m->live);   /* C1 */
}

bool meta_del(struct cap *c)
{
    struct obj_meta *m = obj_meta(c);

    if (!m)
        return false;
    KASSERT(m->live > 0);   /* C2 */
    if (counts_rights(c->type)) {
        if (c->rights & CAP_R_READ) {
            KASSERT(m->n_read > 0);
            m->n_read--;
        }
        if (c->rights & CAP_R_WRITE) {
            KASSERT(m->n_write > 0);
            m->n_write--;
        }
    }
    if (cap_is_ipcbuf_binding(c)) {
        KASSERT(m->alias_w > 0);
        m->alias_w--;
    }
    if (--m->live)
        return false;

    /* The last record: every count it backed is gone with it. A frozen
     * object cannot lose its last record (SC22b F5). Clear the entry so
     * that nothing stale survives into the memory's next object. */
    KASSERT(m->n_read == 0 && m->n_write == 0 && !(m->flags & OBJ_FROZEN));
    memset(m, 0, sizeof(*m));
    return true;
}

void meta_alias(struct cap *frame, cap_rights_t rights, int delta)
{
    struct obj_meta *m = obj_meta(frame);

    KASSERT(frame->type == CAP_FRAME && m && (delta == 1 || delta == -1));
    if (rights & CAP_R_WRITE) {
        KASSERT(delta > 0 ? m->alias_w < m->live : m->alias_w > 0);
        m->alias_w += (uint32_t)delta;
    }
    if (rights & CAP_R_EXEC) {
        KASSERT(delta > 0 ? m->alias_x < m->live : m->alias_x > 0);
        m->alias_x += (uint32_t)delta;
    }
}

bool meta_alias_conflict(const struct cap *frame, cap_rights_t want)
{
    const struct obj_meta *m = obj_meta(frame);

    KASSERT(frame->type == CAP_FRAME && m);
    /* V6, P5: never writable through one alias and executable through
     * another. */
    return ((want & CAP_R_WRITE) && m->alias_x) || ((want & CAP_R_EXEC) && m->alias_w);
}

#ifdef TARKA_SELFTEST
void objmeta_test_freeze(const struct cap *c, bool frozen)
{
    struct obj_meta *m = obj_meta(c);

    KASSERT(m != NULL);
    if (frozen)
        m->flags |= OBJ_FROZEN;
    else
        m->flags &= ~OBJ_FROZEN;
}
#endif
