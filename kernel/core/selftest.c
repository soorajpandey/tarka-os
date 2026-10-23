/*
 * Boot-time kernel self-tests, built only with `make test`. Each test is a
 * plain function; CHECK() records failures without stopping the run so one
 * boot reports every broken invariant.
 */

#ifdef TARKA_SELFTEST

#include <tarka/selftest.h>

#include <tarka/cap.h>
#include <tarka/ipc.h>
#include <tarka/endpoint.h>
#include <tarka/notification.h>
#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/cont.h>
#include <tarka/pmm.h>
#include <tarka/reaper.h>
#include <tarka/string.h>
#include <tarka/thread.h>
#include <tarka/uaccess.h>
#include <tarka/untyped.h>
#include <tarka/user.h>
#include <tarka/syscall.h>
#include <tarka/vspace.h>

#include "cap_internal.h"

#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/paging.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/lapic.h"
#include "../arch/x86_64/percpu.h"
#include "../arch/x86_64/qemu.h"
#include "../arch/x86_64/stacks.h"

static unsigned checks, failures;

/* ADR 0010 gate SC31.3: the independent counter oracle (defined near the
 * end of this file, after every record store). */
static bool meta_audit(void);

void selftest_check(bool ok, const char *expr, const char *file, int line)
{
    checks++;
    if (!ok) {
        failures++;
        kprintf("    FAIL %s (%s:%d)\n", expr, file, line);
    }
}

static void test_printf(void)
{
    char b[32];

    CHECK(ksnprintf(b, sizeof(b), "%d %u %x", -42, 42u, 0xbeefu) == 11);
    CHECK(strcmp(b, "-42 42 beef") == 0);
    ksnprintf(b, sizeof(b), "%#lx|%5s|%-3d|", 0x1234ul, "ab", 7);
    CHECK(strcmp(b, "0x1234|   ab|7  |") == 0);
    ksnprintf(b, sizeof(b), "%08x", 0xabcu);
    CHECK(strcmp(b, "00000abc") == 0);

    /* Truncation must never overflow and must always terminate. */
    memset(b, 'Z', sizeof(b));
    int n = ksnprintf(b, 4, "%s", "overflow");
    CHECK(n == 8);
    CHECK(strcmp(b, "ove") == 0);
    CHECK(b[4] == 'Z');
}

static void test_string(void)
{
    char b[8] = "abcdef";

    memmove(b + 1, b, 5);
    CHECK(memcmp(b, "aabcde", 6) == 0);
    memmove(b, b + 1, 5);
    CHECK(memcmp(b, "abcdee", 6) == 0);
    CHECK(strnlen("abc", 2) == 2);
}

static void test_overflow_helpers(void)
{
    uint64_t r;

    CHECK(add_overflow(UINT64_MAX, 1ull, &r));
    CHECK(!add_overflow(1ull, 2ull, &r) && r == 3);
    CHECK(mul_overflow(UINT64_MAX / 2, 3ull, &r));
}

static void test_pmm(void)
{
    uint64_t before = pmm_free_count();
    /* Raw values: this test does arithmetic on physical addresses. */
    uint64_t a = pa_raw(pmm_alloc());
    uint64_t b = pa_raw(pmm_alloc());

    CHECK(a != 0 && b != 0 && a != b);
    CHECK(a % PAGE_SIZE == 0 && b % PAGE_SIZE == 0);
    CHECK(pmm_free_count() == before - 2);

    /* Fresh frames never contain stale data. */
    const uint8_t *q = phys_to_virt(pa(b));
    bool fresh = true;
    for (size_t i = 0; i < PAGE_SIZE; i++)
        fresh &= q[i] == 0;
    CHECK(fresh);

    /* Freed frames are scrubbed. */
    memset(phys_to_virt(pa(a)), 0xAA, PAGE_SIZE);
    CHECK(pmm_free(pa(a)) == T_OK);
    const uint8_t *p = phys_to_virt(pa(a));
    bool zeroed = true;
    for (size_t i = 0; i < PAGE_SIZE; i++)
        zeroed &= p[i] == 0;
    CHECK(zeroed);

    CHECK(pmm_free(pa(a)) == T_EDOUBLEFREE);
    CHECK(pmm_free(pa(b + 1)) == T_EINVAL);
    CHECK(pmm_free(pa(0)) == T_EINVAL);
    CHECK(pmm_free(pa(~0ull & ~(PAGE_SIZE - 1))) == T_EINVAL);
    /* Frames the allocator never owned (legacy VGA hole, always reserved
     * on PC) cannot be pushed into the free pool. */
    CHECK(pmm_free(pa(0xA0000)) == T_EINVAL);
    CHECK(pmm_free(pa(b)) == T_OK);
    CHECK(pmm_free_count() == before);

    /* Leak check: allocate and release many frames, nothing may be lost. */
    paddr_t frames[64];
    for (size_t i = 0; i < ARRAY_SIZE(frames); i++)
        frames[i] = pmm_alloc();
    for (size_t i = 0; i < ARRAY_SIZE(frames); i++)
        CHECK(!pa_is_null(frames[i]) && pmm_free(frames[i]) == T_OK);
    CHECK(pmm_free_count() == before);
}

static struct cap caps_store[8];   /* 1 KiB: not on the stack (T33) */

static void test_caps(void)
{
    struct cap *storage = caps_store;
    struct cspace cs;
    const struct cap *c;

    cspace_init(&cs, storage, ARRAY_SIZE(caps_store));

    CHECK(cap_install(&cs, 0, CAP_TEST_TOKEN, CAP_R_READ | CAP_R_WRITE | CAP_R_GRANT, 0x1000) == T_OK);
    CHECK(cap_install(&cs, 0, CAP_TEST_TOKEN, CAP_R_READ, 0x2000) == T_EBUSY);
    CHECK(cap_install(&cs, 8, CAP_TEST_TOKEN, CAP_R_READ, 0) == T_ERANGE);
    CHECK(cap_install(&cs, 1, CAP_NULL, CAP_R_READ, 0) == T_EINVAL);
    CHECK(cap_install(&cs, 1, CAP_TEST_TOKEN, 1u << 31, 0) == T_EINVAL);

    /* Lookup enforces type and rights. */
    CHECK(cap_lookup(&cs, 0, CAP_TEST_TOKEN, CAP_R_WRITE, &c) == T_OK && c->object == 0x1000);
    CHECK(cap_lookup(&cs, 0, CAP_IRQ, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 0, CAP_TEST_TOKEN, CAP_R_EXEC, &c) == T_EPERM);
    CHECK(cap_lookup(&cs, 5, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 0xFFFFFFFFu, CAP_TEST_TOKEN, 0, &c) == T_ERANGE);

    /* Derivation can only narrow rights, never widen them. */
    CHECK(cap_derive(&cs, 0, 1, CAP_R_READ | CAP_R_EXEC, 0) == T_OK);
    CHECK(cap_lookup(&cs, 1, CAP_TEST_TOKEN, CAP_R_READ, &c) == T_OK);
    CHECK(cap_lookup(&cs, 1, CAP_TEST_TOKEN, CAP_R_EXEC, &c) == T_EPERM);
    CHECK(cap_lookup(&cs, 1, CAP_TEST_TOKEN, CAP_R_WRITE, &c) == T_EPERM);

    /* Without GRANT a capability cannot be copied onward. */
    CHECK(cap_derive(&cs, 1, 2, CAP_R_ALL, 0) == T_EPERM);
    CHECK(cap_derive(&cs, 0, 1, CAP_R_READ, 0) == T_EBUSY);

    /* Badges are write-once. */
    CHECK(cap_derive(&cs, 0, 3, CAP_R_ALL, 0xB1) == T_OK);
    CHECK(cap_lookup(&cs, 3, CAP_TEST_TOKEN, 0, &c) == T_OK && c->badge == 0xB1);
    CHECK(cap_derive(&cs, 3, 4, CAP_R_ALL, 0xB2) == T_EPERM);
    CHECK(cap_derive(&cs, 3, 4, CAP_R_READ, 0) == T_OK);
    CHECK(cap_lookup(&cs, 4, CAP_TEST_TOKEN, 0, &c) == T_OK && c->badge == 0xB1);

    CHECK(cap_delete(&cs, 1) == T_OK);
    CHECK(cap_delete(&cs, 1) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 1, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);

    /* cap_identify exposes type, rights and badge only. */
    struct cap_info info;
    memset(&info, 0xEE, sizeof(info));
    CHECK(cap_identify(&cs, 3, &info) == T_OK);
    CHECK(info.type == CAP_TEST_TOKEN && info.rights == (CAP_R_READ | CAP_R_WRITE | CAP_R_GRANT) &&
          info.badge == 0xB1);
    CHECK(cap_identify(&cs, 1, &info) == T_EBADCAP);

    cspace_destroy(&cs);
}

/* ---- CDT helpers for tests (ADR 0010, SC20) ---- */

/* The parent of c: the record whose bracket pair most tightly encloses
 * c's (NULL for a root). Walks backwards, skipping complete pairs. */
static const struct cap *cdt_parent_of(const struct cap *c)
{
    unsigned skip = 0;

    for (const struct cdt_elem *e = c->open.prev; e; e = e->prev) {
        if (!cdt_elem_is_open(e))
            skip++;
        else if (skip)
            skip--;
        else
            return cdt_elem_cap(e);
    }
    return NULL;
}

static const struct cdt_elem *cdt_list_head(const struct cap *c)
{
    const struct cdt_elem *e = &c->open;

    while (e->prev)
        e = e->prev;
    return e;
}

/* One whole list is a well-nested bracket sequence: links symmetric, every
 * element belongs to a live record, every close matches the innermost
 * open, nothing left open at the end. */
#define CDT_WALK_MAX 8192
static const struct cap *cdt_stack[CDT_WALK_MAX];

static bool cdt_list_ok(const struct cdt_elem *head)
{
    const struct cdt_elem *prev = NULL;
    unsigned depth = 0, n = 0;

    for (const struct cdt_elem *e = head; e; prev = e, e = e->next) {
        const struct cap *c = cdt_elem_cap(e);
        if (e->prev != prev || ++n > 2 * CDT_WALK_MAX || c->type == CAP_NULL)
            return false;
        if (cdt_elem_is_open(e)) {
            if (depth == CDT_WALK_MAX)
                return false;
            cdt_stack[depth++] = c;
        } else if (!depth || cdt_stack[--depth] != c) {
            return false;
        }
    }
    return depth == 0;
}

/* Every record in a cspace that is not a CNode object: empty slots fully
 * zeroed (no stale links, data or metadata), reserved fields zero, the
 * Untyped-only metadata zero in every other record, and its list
 * well-nested. */
static bool cdt_consistent(const struct cspace *cs)
{
    static const struct cap zero;
    static const struct obj_meta zero_meta;

    for (uint32_t i = 0; i < cs->nslots; i++) {
        const struct cap *c = &cs->slots[i];
        if (c->type == CAP_NULL) {
            if (memcmp(c, &zero, sizeof(zero)) != 0)
                return false;
            continue;
        }
        if (c->slot_flags != 0 || c->slot_aux != 0)
            return false;
        if (memcmp(&c->slot_meta, &zero_meta, sizeof(zero_meta)) != 0)
            return false;
        static const struct ut_meta zero_ut;
        if (c->type != CAP_UNTYPED && memcmp(&c->ut, &zero_ut, sizeof(zero_ut)) != 0)
            return false;
        if (!cdt_list_ok(cdt_list_head(c)))
            return false;
    }
    return true;
}

static bool elem_within(const struct cdt_elem *e, const struct cap *lo, const struct cap *hi)
{
    return !e || (cdt_elem_cap(e) >= lo && cdt_elem_cap(e) < hi);
}

/* No CDT link of a record in `cs` leads outside [lo, hi). */
static bool links_within(const struct cspace *cs, const struct cap *lo, const struct cap *hi)
{
    for (uint32_t i = 0; i < cs->nslots; i++) {
        const struct cap *c = &cs->slots[i];
        if (c->type == CAP_NULL)
            continue;
        if (!elem_within(c->open.prev, lo, hi) || !elem_within(c->open.next, lo, hi) ||
            !elem_within(c->close.prev, lo, hi) || !elem_within(c->close.next, lo, hi))
            return false;
    }
    return true;
}

static unsigned finalised;
static uint64_t finalised_object;

static void count_finaliser(enum cap_type type, uint64_t object)
{
    (void)type;
    finalised++;
    finalised_object = object;
}

static struct cap cdt_store_a[8], cdt_store_b[8];

static void test_cap_revoke(void)
{
    struct cspace a, b;
    const struct cap *c;

    cspace_init(&a, cdt_store_a, ARRAY_SIZE(cdt_store_a));
    cspace_init(&b, cdt_store_b, ARRAY_SIZE(cdt_store_b));
    finalised = 0;
    cap_set_finaliser(CAP_TEST_TOKEN, count_finaliser);

    /* root(a0) ─┬─ a3
     *           └─ a1 ─┬─ b0   (delegated into another cspace)
     *                  └─ a2                                      */
    CHECK(cap_install(&a, 0, CAP_TEST_TOKEN, CAP_R_ALL, 0xE1) == T_OK);
    CHECK(cap_derive(&a, 0, 1, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive(&a, 1, 2, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive_to(&a, 1, &b, 0, CAP_R_READ | CAP_R_GRANT, 0x77) == T_OK);
    CHECK(cap_derive(&a, 0, 3, CAP_R_READ, 0) == T_OK);
    CHECK(cdt_consistent(&a) && cdt_consistent(&b));
    CHECK(cdt_parent_of(&cdt_store_a[1]) == &cdt_store_a[0] &&
          cdt_parent_of(&cdt_store_a[2]) == &cdt_store_a[1] &&
          cdt_parent_of(&cdt_store_b[0]) == &cdt_store_a[1] &&
          cdt_parent_of(&cdt_store_a[3]) == &cdt_store_a[0]);

    /* Revoking a1 removes its descendants in both cspaces, keeps a1, and
     * frees nothing because the object is still referenced. */
    CHECK(cap_revoke(&a, 1, 0) == T_OK);
    CHECK(cap_lookup(&a, 2, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&b, 0, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&a, 1, CAP_TEST_TOKEN, CAP_R_ALL, &c) == T_OK);
    CHECK(cap_lookup(&a, 3, CAP_TEST_TOKEN, CAP_R_READ, &c) == T_OK);
    CHECK(finalised == 0);
    CHECK(cdt_consistent(&a) && cdt_consistent(&b));

    /* Deleting a parent re-parents its children rather than orphaning
     * them: a later revoke of the grandparent still reaches them. */
    CHECK(cap_derive(&a, 1, 4, CAP_R_ALL, 0) == T_OK);   /* a0 > a1 > a4 */
    CHECK(cap_delete(&a, 1) == T_OK);                     /* a0 > a4 */
    CHECK(cdt_parent_of(&cdt_store_a[4]) == &cdt_store_a[0]);
    CHECK(cap_revoke(&a, 0, 0) == T_OK);
    CHECK(cap_lookup(&a, 4, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&a, 3, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);
    CHECK(finalised == 0);

    /* Deleting the last capability runs the finaliser exactly once. */
    CHECK(cap_delete(&a, 0) == T_OK);
    CHECK(finalised == 1 && finalised_object == 0xE1);

    /* Revocation is resumable: with a budget it makes bounded progress.
     * It deletes the first child each step (ADR 0010, SC22), so the top
     * of the chain goes first and the rest re-parents to the root. */
    CHECK(cap_install(&a, 0, CAP_TEST_TOKEN, CAP_R_ALL, 0xE2) == T_OK);
    for (cap_slot_t i = 1; i < 8; i++)
        CHECK(cap_derive(&a, i - 1, i, CAP_R_ALL, 0) == T_OK);  /* chain of 7 */
    CHECK(cap_revoke(&a, 0, 3) == T_EAGAIN);
    for (cap_slot_t i = 1; i <= 3; i++)
        CHECK(cap_lookup(&a, i, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);
    for (cap_slot_t i = 4; i <= 7; i++)
        CHECK(cap_lookup(&a, i, CAP_TEST_TOKEN, 0, &c) == T_OK);
    CHECK(cdt_parent_of(&cdt_store_a[4]) == &cdt_store_a[0]);
    CHECK(cdt_parent_of(&cdt_store_a[5]) == &cdt_store_a[4]);
    CHECK(cdt_consistent(&a));
    CHECK(cap_revoke(&a, 0, 3) == T_EAGAIN);
    CHECK(cap_revoke(&a, 0, 3) == T_OK);
    CHECK(cap_lookup(&a, 1, CAP_TEST_TOKEN, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&a, 0, CAP_TEST_TOKEN, 0, &c) == T_OK);
    CHECK(finalised == 1);

    /* Destroying a cspace must leave no CDT link pointing into it. */
    CHECK(cap_derive_to(&a, 0, &b, 1, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive_to(&b, 1, &a, 1, CAP_R_ALL, 0) == T_OK);  /* a0 > b1 > a1 */
    cspace_destroy(&b);
    CHECK(links_within(&a, cdt_store_a, cdt_store_a + ARRAY_SIZE(cdt_store_a)));
    CHECK(cdt_parent_of(&cdt_store_a[1]) == &cdt_store_a[0]);
    CHECK(cdt_consistent(&a));
    CHECK(finalised == 1);

    cspace_destroy(&a);
    CHECK(finalised == 2 && finalised_object == 0xE2);
    for (size_t i = 0; i < ARRAY_SIZE(cdt_store_a); i++)
        CHECK(cdt_store_a[i].type == CAP_NULL && cdt_store_b[i].type == CAP_NULL);

    cap_set_finaliser(CAP_TEST_TOKEN, NULL);
}

/* ---- Untyped and retype (docs/adr/0006-untyped-retype.md) ---- */

static struct cap ut_store[128];
static struct cspace uts;

static bool is_free_slot(const struct cspace *cs, cap_slot_t s)
{
    return cs->slots[s].type == CAP_NULL;
}

/* Carve a test Untyped of 2^bits bytes out of a root Untyped into uts
 * slot 0. Returns false if no root Untyped is large enough. */
static bool ut_setup(unsigned bits)
{
    struct cspace *root = untyped_root_cspace();

    cspace_init(&uts, ut_store, ARRAY_SIZE(ut_store));
    for (cap_slot_t r = 0; r < root->nslots; r++) {
        if (root->slots[r].type == CAP_UNTYPED && cap_obj_bits(&root->slots[r]) > bits)
            return cap_retype(root, r, CAP_UNTYPED, bits, &uts, 0, 1) == T_OK;
    }
    return false;
}

/* Let the reaper finish every dying object (SC24): what a test means by
 * "and then the memory is free again" (R4, amended O7). Bounded: the
 * reaper panics if it does not end. */
static void reap_all(void)
{
    (void)reaper_drain(1ul << 24);
}

/* Revoke completely: T_EAGAIN means an Untyped below still has dying
 * objects (amended O8), which the reaper finishes. */
static int revoke_all(struct cspace *cs, cap_slot_t slot)
{
    int rc;

    while ((rc = cap_revoke(cs, slot, 0)) == T_EAGAIN)
        reap_all();
    reap_all();
    return rc;
}

static void ut_teardown(void)
{
    cspace_destroy(&uts); /* revokes the test Untyped, then deletes it */
}

/* Raw physical address, for test arithmetic only. */
static uint64_t slot_paddr(cap_slot_t s) { return pa_raw(cap_obj_paddr(&ut_store[s])); }

static bool page_is(uint64_t p, uint8_t v)
{
    const uint8_t *b = phys_to_virt(pa(p));
    for (size_t i = 0; i < PAGE_SIZE; i++)
        if (b[i] != v)
            return false;
    return true;
}

static bool frames_conserved(void)
{
    return untyped_boot_frames() + pmm_free_count() + pmm_kernel_count() == pmm_total_count();
}

/* O1: every frame has exactly one owner, and no memory went missing. */
static void test_untyped_ownership(void)
{
    struct cspace *root = untyped_root_cspace();
    uint64_t frames = 0;
    bool owned_once = true, disjoint = true;

    for (cap_slot_t i = 0; i < root->nslots; i++) {
        const struct cap *a = &root->slots[i];
        if (a->type != CAP_UNTYPED)
            continue;
        uint64_t base = pa_raw(cap_obj_paddr(a));
        uint64_t size = 1ull << cap_obj_bits(a);

        frames += size / PAGE_SIZE;
        if (base % size)
            disjoint = false; /* not naturally aligned */
        for (uint64_t p = base; p < base + size; p += PAGE_SIZE)
            owned_once &= !pmm_frame_is_free(pa(p));
        for (cap_slot_t j = i + 1; j < root->nslots; j++) {
            const struct cap *b = &root->slots[j];
            if (b->type != CAP_UNTYPED)
                continue;
            uint64_t bb = pa_raw(cap_obj_paddr(b));
            if (base < bb + (1ull << cap_obj_bits(b)) && bb < base + size)
                disjoint = false;
        }
    }
    CHECK(frames == untyped_boot_frames() && frames > 0);
    CHECK(owned_once);   /* no Untyped frame can also be allocated by pmm */
    CHECK(disjoint);     /* no frame belongs to two root Untypeds */
    /* Conservation: every frame is free, held by the kernel, or Untyped. */
    CHECK(frames + pmm_free_count() + pmm_kernel_count() == pmm_total_count());
}

/* O2-O4: authority and validation; failed calls change nothing. */
static void test_untyped_rules(void)
{
    struct cap_info info;

    CHECK(ut_setup(16));
    uint64_t base = slot_paddr(0);

    /* Memory authority cannot be duplicated. */
    CHECK(cap_derive(&uts, 0, 1, CAP_R_ALL, 0) == T_EINVAL);

    /* Holding an Untyped without WRITE confers no right to create. The
     * fabricated capability is never successfully used, so it owns
     * nothing. */
    CHECK(cap_install(&uts, 90, CAP_UNTYPED, CAP_R_READ, cap_obj_encode(pa(base), 16)) == T_OK);
    CHECK(cap_retype(&uts, 90, CAP_FRAME, 0, &uts, 1, 1) == T_EPERM);
    CHECK(cap_delete(&uts, 90) == T_OK);

    /* No Untyped, no objects. */
    CHECK(cap_retype(&uts, 5, CAP_FRAME, 0, &uts, 1, 1) == T_EBADCAP);
    CHECK(cap_retype(&uts, 999, CAP_FRAME, 0, &uts, 1, 1) == T_ERANGE);

    /* Invalid requests. */
    CHECK(cap_retype(&uts, 0, CAP_NULL, 0, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_IRQ, 0, &uts, 1, 1) == T_EINVAL);   /* not retypeable */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 1, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &uts, 1, 1) == T_EINVAL); /* same size */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 17, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 11, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 0, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 13, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, 0) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, RETYPE_MAX_COUNT + 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 0xFFFFFFFFu, 2) == T_ERANGE);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, ARRAY_SIZE(ut_store) - 1, 2) == T_ERANGE);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 0, 1) == T_EBUSY);   /* occupied */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, 17) == T_ENOMEM); /* 16 frames fit */

    /* None of the failures moved the watermark or created anything. */
    CHECK(!cap_has_children(&ut_store[0]));
    for (cap_slot_t i = 1; i < ARRAY_SIZE(ut_store); i++)
        CHECK(is_free_slot(&uts, i));
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, 1) == T_OK);
    CHECK(slot_paddr(1) == base && cap_obj_bits(&ut_store[1]) == FRAME_BITS);
    CHECK(ut_store[1].rights == CAP_R_ALL);

    /* Untypeds report no badge; their watermark is not a badge. */
    CHECK(cap_identify(&uts, 0, &info) == T_OK && info.badge == 0 &&
          info.rights == (CAP_R_READ | CAP_R_WRITE));

    ut_teardown();
}

/* O6, O7: exhaustion, zeroing, and exactly when memory is reusable. */
static void test_untyped_exhaustion(void)
{
    CHECK(ut_setup(16)); /* 16 frames */
    uint64_t base = slot_paddr(0);

    memset(phys_to_virt(pa(base)), 0xAA, 16 * PAGE_SIZE);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, 16) == T_OK);
    bool zeroed = true, placed = true;
    for (cap_slot_t i = 1; i <= 16; i++) {
        zeroed &= page_is(slot_paddr(i), 0);
        placed &= slot_paddr(i) >= base && slot_paddr(i) < base + 16 * PAGE_SIZE &&
                  slot_paddr(i) % PAGE_SIZE == 0;
    }
    CHECK(zeroed);  /* O6: previous contents never visible */
    CHECK(placed);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 17, 1) == T_ENOMEM);

    /* O7: freeing one of many does not make memory reusable... */
    CHECK(cap_delete(&uts, 5) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 17, 1) == T_ENOMEM);

    /* ...freeing all of them does. */
    memset(phys_to_virt(pa(slot_paddr(1))), 0x55, PAGE_SIZE);
    CHECK(cap_revoke(&uts, 0, 0) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, 16) == T_OK);
    CHECK(slot_paddr(1) == base || slot_paddr(16) == base);
    zeroed = true;
    for (cap_slot_t i = 1; i <= 16; i++)
        zeroed &= page_is(slot_paddr(i), 0);
    CHECK(zeroed);

    /* Fine-grained reuse: split into one-frame Untypeds first. */
    CHECK(cap_revoke(&uts, 0, 0) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 12, &uts, 1, 16) == T_OK);
    for (cap_slot_t i = 1; i <= 16; i++)
        CHECK(cap_retype(&uts, i, CAP_FRAME, 0, &uts, 20 + i, 1) == T_OK);
    CHECK(cap_retype(&uts, 1, CAP_FRAME, 0, &uts, 40, 1) == T_ENOMEM);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 40, 1) == T_ENOMEM);
    CHECK(cap_delete(&uts, 21) == T_OK);                     /* child 1's only object */
    CHECK(cap_retype(&uts, 1, CAP_FRAME, 0, &uts, 40, 1) == T_OK);
    CHECK(slot_paddr(40) == slot_paddr(1));

    /* O8: an Untyped with children cannot be deleted. */
    CHECK(cap_delete(&uts, 1) == T_EBUSY);
    CHECK(cap_delete(&uts, 0) == T_EBUSY);
    CHECK(cap_delete(&uts, 40) == T_OK);
    CHECK(cap_delete(&uts, 1) == T_OK); /* childless: allowed */

    ut_teardown();
}

/* O5: random retype sequences never produce overlapping objects. */
static struct { uint64_t base; uint64_t size; int parent; } objs[ARRAY_SIZE(ut_store)];

static uint64_t rng_state;
static uint32_t rng(uint32_t n)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state % n);
}

static void test_untyped_overlap(void)
{
    bool ok = true;
    unsigned created = 0;

    CHECK(ut_setup(18)); /* 256 KiB */
    rng_state = 0x7a72a05ull;

    for (int round = 0; round < 8; round++) {
        CHECK(revoke_all(&uts, 0) == T_OK);
        uint32_t nobj = 0;
        objs[0].base = slot_paddr(0);
        objs[0].size = 1ull << 18;
        objs[0].parent = -1;
        nobj = 1;

        for (cap_slot_t dst = 1; dst < ARRAY_SIZE(ut_store);) {
            /* Source: the test Untyped or any child Untyped made so far. */
            uint32_t src = rng(nobj);
            if (ut_store[src].type != CAP_UNTYPED)
                src = 0;
            unsigned src_bits = cap_obj_bits(&ut_store[src]);

            enum cap_type t;
            unsigned sb = 0;
            switch (rng(4)) {
            case 0: t = CAP_FRAME; break;
            case 1: t = CAP_ENDPOINT; break;
            case 2: t = CAP_CNODE; sb = 1 + rng(4); break;
            default:
                t = CAP_UNTYPED;
                if (src_bits <= 12)
                    continue;
                sb = 12 + rng(src_bits - 12 < 3 ? src_bits - 12 : 3);
                break;
            }
            uint32_t count = 1 + rng(4);
            if (dst + count > ARRAY_SIZE(ut_store))
                break;

            int rc = cap_retype(&uts, src, t, sb, &uts, dst, count);
            if (rc == T_EAGAIN) {   /* a reset waits for the reaper (SC23) */
                reap_all();
                continue;
            }
            if (rc == T_ENOMEM || rc == T_EINVAL) {
                if (rc == T_ENOMEM && src == 0)
                    break;
                continue;
            }
            CHECK(rc == T_OK);
            for (uint32_t i = 0; i < count; i++, dst++) {
                objs[dst].base = slot_paddr(dst);
                objs[dst].size = 1ull << cap_obj_bits(&ut_store[dst]);
                objs[dst].parent = (int)src;
            }
            nobj = dst;
            created += count;
        }

        /* Every object is aligned to its size and inside its parent, and
         * objects sharing a parent never overlap. */
        for (uint32_t i = 1; i < nobj; i++) {
            const typeof(objs[0]) *a = &objs[i], *p = &objs[a->parent];
            ok &= a->base % a->size == 0;
            ok &= a->base >= p->base && a->base + a->size <= p->base + p->size;
            for (uint32_t j = i + 1; j < nobj; j++) {
                const typeof(objs[0]) *b = &objs[j];
                if (a->parent == b->parent)
                    ok &= a->base + a->size <= b->base || b->base + b->size <= a->base;
            }
        }
        CHECK(cdt_consistent(&uts));
    }
    CHECK(ok);
    CHECK(created > 200); /* the generator really exercised retype */

    ut_teardown();
}

/* O9-O11: revocation reaches everything built from an Untyped, including
 * through CNode finalisers that delete capabilities mid-revocation. */
static void test_untyped_revoke(void)
{
    struct cspace cn;

    CHECK(ut_setup(16));

    /* U > { C > F > F'(inside CN), CN }, arranged so that CN is deleted
     * first and its finaliser removes F', the node revocation would visit
     * next. */
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 3, &uts, 3, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 14, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 1, CAP_FRAME, 0, &uts, 2, 1) == T_OK);
    CHECK(cnode_view(&uts, 3, &cn) == T_OK && cn.nslots == 8);
    CHECK(cap_derive_to(&uts, 2, &cn, 0, CAP_R_READ, 0) == T_OK);
    /* An Untyped can also live inside a CNode. */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 12, &cn, 1, 1) == T_OK);
    CHECK(cap_retype(&cn, 1, CAP_ENDPOINT, 0, &cn, 2, 1) == T_OK);

    CHECK(cap_revoke(&uts, 0, 0) == T_OK);
    for (cap_slot_t i = 1; i < ARRAY_SIZE(ut_store); i++)
        CHECK(is_free_slot(&uts, i));
    CHECK(!cap_has_children(&ut_store[0]));
    CHECK(cdt_consistent(&uts));

    /* Deleting a CNode's last capability deletes what it holds and leaves
     * no CDT link into its memory. */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 2, &uts, 2, 1) == T_OK);
    CHECK(cnode_view(&uts, 2, &cn) == T_OK);
    struct cap *cn_lo = cn.slots, *cn_hi = cn.slots + cn.nslots;
    CHECK(cap_derive_to(&uts, 1, &cn, 0, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive_to(&cn, 0, &cn, 3, CAP_R_READ, 0) == T_OK);
    CHECK(cap_delete(&uts, 2) == T_OK);
    /* SC23: the CNode is dying; the reaper empties it later. Until then
     * its slots still hold their records (nobody can reach them). */
    CHECK(cap_has_children(&ut_store[1]));
    reap_all();
    CHECK(!cap_has_children(&ut_store[1]));
    CHECK(!(cdt_elem_cap(ut_store[1].open.next) >= cn_lo && cdt_elem_cap(ut_store[1].open.next) < cn_hi));
    CHECK(!ut_store[1].close.next ||
          !(cdt_elem_cap(ut_store[1].close.next) >= cn_lo && cdt_elem_cap(ut_store[1].close.next) < cn_hi));
    CHECK(cdt_consistent(&uts));

    ut_teardown();
    CHECK(frames_conserved());
}

/* T33: deleting nested CNodes uses constant kernel stack. */
/* ---- The bracket-list CDT against a reference forest (ADR 0010, SC20,
 * test 7, gate SC31.1) ----
 *
 * The model keeps, for every slot of two cspaces, its parent (as a global
 * index), or ROOT, or EMPTY, and its object. Every kernel operation is
 * applied to both, and after each one the parent of every live record,
 * read from the brackets, must equal the model's. */

#define CDT_N     64                 /* slots per cspace; two cspaces */
#define M_EMPTY   (-2)
#define M_ROOT    (-1)
static struct cap cdtm_a[CDT_N], cdtm_b[CDT_N];
static struct cspace cdtm_cs[2];
static int16_t m_par[2 * CDT_N];
static uint16_t m_obj[2 * CDT_N];

static struct cap *m_cap(int g) { return &cdtm_cs[g / CDT_N].slots[g % CDT_N]; }

static int m_index(const struct cap *c)
{
    for (int k = 0; k < 2; k++)
        if (c >= cdtm_cs[k].slots && c < cdtm_cs[k].slots + CDT_N)
            return k * CDT_N + (int)(c - cdtm_cs[k].slots);
    return -100;   /* outside the model: a failure */
}

static void m_reset(void)
{
    /* Delete what the previous round left: re-zeroing live records would
     * leave their objects' counts (and CDT links) stale. */
    cspace_destroy(&cdtm_cs[0]);
    cspace_destroy(&cdtm_cs[1]);
    cspace_init(&cdtm_cs[0], cdtm_a, CDT_N);
    cspace_init(&cdtm_cs[1], cdtm_b, CDT_N);
    for (int g = 0; g < 2 * CDT_N; g++)
        m_par[g] = M_EMPTY;
    finalised = 0;
    cap_set_finaliser(CAP_TEST_TOKEN, count_finaliser);
}

static bool m_root(int g, uint16_t obj)
{
    m_par[g] = M_ROOT;
    m_obj[g] = obj;
    return cap_install(&cdtm_cs[g / CDT_N], g % CDT_N, CAP_TEST_TOKEN, CAP_R_ALL, 0x1000u + obj) == T_OK;
}

static bool m_derive(int src, int dst)
{
    m_par[dst] = (int16_t)src;
    m_obj[dst] = m_obj[src];
    return cap_derive_to(&cdtm_cs[src / CDT_N], src % CDT_N, &cdtm_cs[dst / CDT_N],
                         dst % CDT_N, CAP_R_ALL, 0) == T_OK;
}

/* Deleting x in the model: x's children go to x's parent. Returns whether
 * the kernel should have run the finaliser (no record of x's object
 * left). */
static bool m_forget(int x)
{
    bool last = true;
    for (int g = 0; g < 2 * CDT_N; g++)
        if (m_par[g] == x)
            m_par[g] = m_par[x];
    m_par[x] = M_EMPTY;
    for (int g = 0; g < 2 * CDT_N; g++)
        if (m_par[g] != M_EMPTY && m_obj[g] == m_obj[x])
            last = false;
    return last;
}

static bool m_delete(int x)
{
    unsigned before = finalised;
    uint16_t obj = m_obj[x];
    bool last = m_forget(x);
    if (cap_delete(&cdtm_cs[x / CDT_N], x % CDT_N) != T_OK)
        return false;
    return finalised == before + (last ? 1u : 0u) &&
           (!last || finalised_object == 0x1000u + obj);
}

/* Every slot agrees with the model, and both lists are well-nested. */
static bool m_agrees(void)
{
    for (int g = 0; g < 2 * CDT_N; g++) {
        const struct cap *c = m_cap(g);
        if (m_par[g] == M_EMPTY) {
            if (c->type != CAP_NULL)
                return false;
            continue;
        }
        const struct cap *p = cdt_parent_of(c);
        int want = m_par[g];
        if (want == M_ROOT ? p != NULL : (p == NULL || m_index(p) != want))
            return false;
    }
    return cdt_consistent(&cdtm_cs[0]) && cdt_consistent(&cdtm_cs[1]);
}

static int m_has_child(int p)
{
    for (int g = 0; g < 2 * CDT_N; g++)
        if (m_par[g] == p)
            return 1;
    return 0;
}

/* The tree A{B{C{D},E}} with each deletion order the ADR asks for. */
static void test_cdt_forest(void)
{
    enum { A, B, C, D, E };
    static const int8_t orders[][5] = {
        { B, -1 }, { C, -1 }, { D, -1 }, { E, -1 },        /* each alone */
        { B, C, -1 }, { C, B, -1 }, { D, B, E, -1 },
        { E, D, C, B, -1 }, { B, E, D, C, -1 }, { A, B, -1 }, /* root first */
    };
    for (size_t o = 0; o < ARRAY_SIZE(orders); o++) {
        m_reset();
        CHECK(m_root(A, 1) && m_derive(A, B) && m_derive(B, C) && m_derive(C, D) &&
              m_derive(B, E));
        CHECK(m_agrees());
        for (int k = 0; orders[o][k] >= 0; k++) {
            CHECK(m_delete(orders[o][k]));
            CHECK(m_agrees());
        }
    }

    /* The revision-2 counterexample: z must end up under p. */
    enum { P0 = 0, X1, Y2, Z3, M4, W5 };
    m_reset();
    CHECK(m_root(P0, 2) && m_derive(P0, X1) && m_derive(X1, Y2) && m_derive(Y2, Z3) &&
          m_derive(P0, M4) && m_derive(M4, W5));
    CHECK(m_delete(M4) && m_agrees());
    CHECK(m_delete(Y2) && m_agrees());
    CHECK(m_delete(X1) && m_agrees());
    CHECK(cdt_parent_of(m_cap(Z3)) == m_cap(P0));
    CHECK(cdt_parent_of(m_cap(W5)) == m_cap(P0));
    /* Revoking w must not touch z, which was never derived from it. */
    CHECK(cap_revoke(&cdtm_cs[0], W5, 0) == T_OK && m_cap(Z3)->type == CAP_TEST_TOKEN);

    /* Re-derivation into the same slot, from a different parent, in the
     * other cspace. */
    CHECK(m_delete(Z3) && m_derive(W5, Z3) && m_agrees());
    CHECK(cdt_parent_of(m_cap(Z3)) == m_cap(W5));
    CHECK(m_delete(Z3) && m_derive(P0, CDT_N + 7) && m_derive(CDT_N + 7, Z3) && m_agrees());
    cap_set_finaliser(CAP_TEST_TOKEN, NULL);
}

/* Random derive, delete and one-step revoke across two cspaces and several
 * objects, compared with the model after every step. */
static void test_cdt_random(void)
{
    for (uint64_t seed = 1; seed <= 4; seed++) {
        rng_state = 0x9E3779B97F4A7C15ull * seed;
        m_reset();
        for (int k = 0; k < 4; k++)
            CHECK(m_root(k * 9, (uint16_t)(10 + k)));
        bool ok = true;
        for (int step = 0; step < 400 && ok; step++) {
            static int live[2 * CDT_N], empty[2 * CDT_N];   /* off the stack (T33) */
            int nl = 0, ne = 0;
            for (int g = 0; g < 2 * CDT_N; g++) {
                if (m_par[g] == M_EMPTY)
                    empty[ne++] = g;
                else
                    live[nl++] = g;
            }
            uint32_t r = rng(100);
            if (nl == 0) {
                ok &= m_root(empty[rng((uint32_t)ne)], (uint16_t)(20 + step % 7));
            } else if (r < 50 && ne > 0) {
                ok &= m_derive(live[rng((uint32_t)nl)], empty[rng((uint32_t)ne)]);
            } else if (r < 80) {
                ok &= m_delete(live[rng((uint32_t)nl)]);
            } else {
                /* One revocation step: the victim is the kernel's first
                 * child, which must be one of p's children in the model. */
                int p = live[rng((uint32_t)nl)];
                if (m_has_child(p)) {
                    int v = m_index(cap_first_child(m_cap(p)));
                    ok &= v >= 0 && m_par[v] == p;
                    if (!ok)
                        break;
                    unsigned before = finalised;
                    bool last = m_forget(v);
                    int want = m_has_child(p) ? T_EAGAIN : T_OK;
                    ok &= cap_revoke(&cdtm_cs[p / CDT_N], p % CDT_N, 1) == want;
                    ok &= finalised == before + (last ? 1u : 0u);
                }
            }
            ok &= m_agrees();
        }
        CHECK(ok);
    }
    cap_set_finaliser(CAP_TEST_TOKEN, NULL);
}

/* SC22 and O8: revocation never deletes an Untyped that still has
 * descendants; it descends into it (bounded, because nested Untypeds
 * shrink). U0 > U1 > U2 > F, revoked one step at a time. */
static void test_cdt_untyped_descent(void)
{
    CHECK(ut_setup(16));
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 15, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 1, CAP_UNTYPED, 14, &uts, 2, 1) == T_OK);
    CHECK(cap_retype(&uts, 2, CAP_FRAME, 0, &uts, 3, 1) == T_OK);
    CHECK(cap_derive(&uts, 3, 4, CAP_R_ALL, 0) == T_OK);
    /* Step 1 descends U1 > U2 and deletes F (its copy F' re-parents to
     * U2); step 2 descends again and deletes F'; then U2; then U1. The
     * step counts are the deletion plus one per Untyped descended. */
    static const struct { cap_slot_t gone; uint64_t steps; } plan[] = {
        { 3, 3 }, { 4, 3 }, { 2, 2 }, { 1, 1 },
    };
    for (size_t k = 0; k < ARRAY_SIZE(plan); k++) {
        uint64_t steps = cdt_revoke_steps;
        CHECK(cap_revoke(&uts, 0, 1) == (k + 1 < ARRAY_SIZE(plan) ? T_EAGAIN : T_OK));
        CHECK(cdt_revoke_steps - steps == plan[k].steps);
        CHECK(is_free_slot(&uts, plan[k].gone));
        for (cap_slot_t s = 1; s < plan[k].gone && s < 3; s++)
            CHECK(!is_free_slot(&uts, s));            /* Untypeds above still there */
        CHECK(cdt_consistent(&uts));
    }
    ut_teardown();
    CHECK(frames_conserved());
}

/* Gate SC31.4: one revocation step costs O(1), whatever the subtree's
 * size. A chain of 1500 and a fan of 1500 under one root, revoked with
 * budget 1: each call deletes exactly one record and takes a bounded
 * number of steps; the tree stays valid throughout. A pre-scan of the
 * subtree would cost thousands of steps on the first call. */
#define BIG_N 3072
static struct cap big_store[BIG_N];
static int16_t big_par[BIG_N];

static bool big_agrees(const struct cspace *cs)
{
    /* One linear pass: every record's parent from the bracket nesting. */
    const struct cdt_elem *prev = NULL;
    unsigned depth = 0;
    for (const struct cdt_elem *e = &cs->slots[0].open; e; prev = e, e = e->next) {
        const struct cap *c = cdt_elem_cap(e);
        if (e->prev != prev)
            return false;
        if (cdt_elem_is_open(e)) {
            int want = big_par[c - cs->slots];
            int got = depth ? (int)(cdt_stack[depth - 1] - cs->slots) : M_ROOT;
            if (got != want || depth == CDT_WALK_MAX)
                return false;
            cdt_stack[depth++] = c;
        } else if (!depth || cdt_stack[--depth] != c) {
            return false;
        }
    }
    return depth == 0;
}

static void test_cdt_revoke_bound(void)
{
    struct cspace cs;

    cspace_init(&cs, big_store, BIG_N);
    CHECK(cap_install(&cs, 0, CAP_TEST_TOKEN, CAP_R_ALL, 0x2000) == T_OK);
    big_par[0] = M_ROOT;
    for (int i = 1; i <= 1500; i++) {                 /* chain 0 > 1 > ... > 1500 */
        CHECK(cap_derive(&cs, (cap_slot_t)(i - 1), (cap_slot_t)i, CAP_R_ALL, 0) == T_OK);
        big_par[i] = (int16_t)(i - 1);
    }
    for (int i = 1501; i < BIG_N; i++) {               /* fan under the root */
        CHECK(cap_derive(&cs, 0, (cap_slot_t)i, CAP_R_ALL, 0) == T_OK);
        big_par[i] = 0;
    }
    CHECK(big_agrees(&cs));
    unsigned left = BIG_N - 1;
    uint64_t worst = 0;
    bool ok = true;
    while (left && ok) {
        struct cap *v = cap_first_child(&cs.slots[0]);
        int vi = (int)(v - cs.slots);
        uint64_t steps = cdt_revoke_steps;
        ok &= cap_revoke(&cs, 0, 1) == (left > 1 ? T_EAGAIN : T_OK);
        uint64_t d = cdt_revoke_steps - steps;
        worst = d > worst ? d : worst;
        ok &= cs.slots[vi].type == CAP_NULL;          /* exactly that one went */
        for (int g = 1; g < BIG_N; g++)
            if (big_par[g] == vi)
                big_par[g] = big_par[vi];
        big_par[vi] = M_EMPTY;
        left--;
        if (left % 256 == 0 && left)
            ok &= big_agrees(&cs);
    }
    CHECK(ok && left == 0 && worst == 1);
    CHECK(!cap_has_children(&cs.slots[0]));
    cspace_destroy(&cs);
}

static void test_cnode_chain(void)
{
    struct cspace cur;

    CHECK(ut_setup(18));

    /* A chain of 1000 CNodes, each holding the only capability to the
     * next. The old recursive finaliser needed ~300 bytes of stack per
     * level and would run off the 32 KiB boot stack. */
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 1, &uts, 1, 1) == T_OK);
    CHECK(cnode_view(&uts, 1, &cur) == T_OK);
    int built = 1;
    for (; built < 1000; built++) {
        if (cap_retype(&uts, 0, CAP_CNODE, 1, &cur, 0, 1) != T_OK ||
            cnode_view(&cur, 0, &cur) != T_OK)
            break;
    }
    CHECK(built == 1000);
    CHECK(cap_delete(&uts, 1) == T_OK);
    /* Each CNode dies through the reaper, which meets the next one in
     * slot 0 and lists it: an iterative walk, constant stack (T33). */
    CHECK(cap_has_children(&ut_store[0]) && ut_store[0].ut.dying == 1);
    reap_all();
    CHECK(!cap_has_children(&ut_store[0]) && ut_store[0].ut.dying == 0); /* all 1000 */
    CHECK(cdt_consistent(&uts));

    /* A revocation during the drain deletes the capability held in slot 0
     * of a CNode that is itself still waiting to be drained. Its list link
     * must survive, or the CNodes behind it are never drained (and the
     * endpoint below leaks: the Untyped would keep a child). */
    struct cspace a, b, c;
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 2, &uts, 1, 1) == T_OK);       /* A */
    CHECK(cnode_view(&uts, 1, &a) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 1, &a, 1, 1) == T_OK);         /* B in A[1] */
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 1, &a, 2, 1) == T_OK);         /* C in A[2] */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 12, &a, 3, 1) == T_OK);      /* U2 in A[3] */
    CHECK(cnode_view(&a, 1, &b) == T_OK && cnode_view(&a, 2, &c) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &b, 1, 1) == T_OK);      /* leak detector */
    CHECK(cap_retype(&a, 3, CAP_FRAME, 0, &c, 0, 1) == T_OK);           /* C[0] from U2 */
    /* The reaper lists B and C while emptying A, and revoking U2 deletes
     * C[0] while C is still listed: C's list link (a slot property) must
     * survive, or the endpoint below leaks. */
    CHECK(cap_delete(&uts, 1) == T_OK);
    reap_all();
    CHECK(!cap_has_children(&ut_store[0]) && ut_store[0].ut.dying == 0);
    CHECK(cdt_consistent(&uts));

    ut_teardown();
}

/* ---- Address spaces (docs/adr/0007-address-spaces.md) ---- */

#define VA0 0x0000008000000000ull /* aligned for every table depth */
#define RO  CAP_R_READ
#define RW  (CAP_R_READ | CAP_R_WRITE)
#define RX  (CAP_R_READ | CAP_R_EXEC)

static struct cap b_store[16];
static struct cspace bcs; /* a second task's cspace */

/* AS in slot `as`; PageTables pt..pt+2 installed so VA0's 2 MiB region is
 * mappable. */
static bool build_as(cap_slot_t as, cap_slot_t pt)
{
    return cap_retype(&uts, 0, CAP_ADDRESS_SPACE, 0, &uts, as, 1) == T_OK &&
           cap_retype(&uts, 0, CAP_PAGE_TABLE, 0, &uts, pt, 3) == T_OK &&
           vspace_install(&uts, as, pt, ua(VA0), 1) == T_OK &&
           vspace_install(&uts, as, pt + 1, ua(VA0), 2) == T_OK &&
           vspace_install(&uts, as, pt + 2, ua(VA0), 3) == T_OK;
}

/* Hardware truth: the effective P/W/U/NX flags the MMU sees for `va` with
 * the address space active, or 0 if unmapped. */
static uint64_t hw_flags(const struct cspace *cs, cap_slot_t as, uint64_t va)
{
    uint64_t f = 0;

    if (vspace_activate((struct cspace *)cs, as) != T_OK)
        return 0;
    if (paging_query(va, &f) != 0)
        f = 0;
    vspace_activate_kernel();
    return f;
}

static inline void smap_open(void)  { if (cpu_sec.smap) __asm__ volatile("stac" ::: "memory"); }
static inline void smap_close(void) { if (cpu_sec.smap) __asm__ volatile("clac" ::: "memory"); }

/* Access user memory through the user address, as user code would. */
static uint64_t user_read64(cap_slot_t as, uint64_t va)
{
    uint64_t v;

    if (vspace_activate(&uts, as) != T_OK)
        return 0;
    smap_open();
    v = *(volatile uint64_t *)(uintptr_t)va;
    smap_close();
    vspace_activate_kernel();
    return v;
}

static void user_write64(cap_slot_t as, uint64_t va, uint64_t v)
{
    if (vspace_activate(&uts, as) != T_OK)
        return;
    smap_open();
    *(volatile uint64_t *)(uintptr_t)va = v;
    smap_close();
    vspace_activate_kernel();
}

static bool cr3_is_kernel(uint64_t master)
{
    return (read_cr3() & 0x000FFFFFFFFFF000ull) == master;
}

/* V5, V7 and hardware truth. */
static void test_vspace_perms(void)
{
    CHECK(ut_setup(20));
    CHECK(build_as(1, 2)); /* AS 1, tables 2-4 */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 10, 6) == T_OK); /* frames 10-15 */

    /* Only R, RW and RX are mappings. */
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), 0) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), RW | CAP_R_EXEC) == T_EINVAL);   /* RWX */
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), CAP_R_WRITE | CAP_R_EXEC) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), CAP_R_WRITE) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), CAP_R_EXEC) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), RW | CAP_R_GRANT) == T_EINVAL);
    CHECK(!ut_store[10].map_state);

    CHECK(vspace_map(&uts, 1, 10, ua(VA0 + 0x0000), RO) == T_OK);
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 0x1000), RW) == T_OK);
    CHECK(vspace_map(&uts, 1, 12, ua(VA0 + 0x2000), RX) == T_OK);

    uint64_t fr = hw_flags(&uts, 1, VA0), fw = hw_flags(&uts, 1, VA0 + 0x1000),
             fx = hw_flags(&uts, 1, VA0 + 0x2000);
    CHECK((fr & PTE_P) && (fr & PTE_U) && !(fr & PTE_W) && (fr & PTE_NX));
    CHECK((fw & PTE_P) && (fw & PTE_U) && (fw & PTE_W) && (fw & PTE_NX));
    CHECK((fx & PTE_P) && (fx & PTE_U) && !(fx & PTE_W) && !(fx & PTE_NX));
    CHECK(hw_flags(&uts, 1, VA0 + 0x3000) == 0);

    /* The user address really reaches the frame named by the capability. */
    *(volatile uint64_t *)phys_to_virt(cap_obj_paddr(&ut_store[10])) = 0x7a72a0d5ull;
    CHECK(user_read64(1, VA0) == 0x7a72a0d5ull);
    user_write64(1, VA0 + 0x1000, 0x5eed);
    CHECK(*(volatile uint64_t *)phys_to_virt(cap_obj_paddr(&ut_store[11])) == 0x5eed);

    /* V7: in place, rights only shrink. */
    CHECK(vspace_protect(&uts, 11, RX) == T_EPERM);   /* RW -> RX */
    CHECK(vspace_protect(&uts, 12, RW) == T_EPERM);   /* RX -> RW */
    CHECK(vspace_protect(&uts, 10, RW) == T_EPERM);   /* R  -> RW */
    CHECK(vspace_protect(&uts, 11, RW | CAP_R_EXEC) == T_EPERM);
    CHECK(vspace_protect(&uts, 11, 0) == T_EPERM);
    CHECK(vspace_protect(&uts, 11, RO) == T_OK);      /* RW -> R */
    CHECK(vspace_protect(&uts, 12, RO) == T_OK);      /* RX -> R */
    CHECK(vspace_protect(&uts, 13, RO) == T_EINVAL);  /* not mapped */
    fw = hw_flags(&uts, 1, VA0 + 0x1000);
    fx = hw_flags(&uts, 1, VA0 + 0x2000);
    CHECK((fw & PTE_P) && !(fw & PTE_W) && (fw & PTE_NX));
    CHECK((fx & PTE_P) && !(fx & PTE_W) && (fx & PTE_NX));

    /* V8: write-then-execute is a fresh map, needing WRITE+EXEC authority. */
    CHECK(vspace_unmap(&uts, 11) == T_OK);
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 0x1000), RX) == T_OK);
    CHECK(cap_derive(&uts, 13, 20, RW | CAP_R_GRANT, 0) == T_OK);   /* no EXEC */
    CHECK(vspace_map(&uts, 1, 20, ua(VA0 + 0x4000), RX) == T_EPERM);
    CHECK(vspace_map(&uts, 1, 20, ua(VA0 + 0x4000), RW) == T_OK);

    CHECK(vspace_audit(&uts, 1));
    ut_teardown();
}

/* V6: one physical frame is never writable through one mapping and
 * executable through another, in any address space. */
static void test_vspace_alias(void)
{
    CHECK(ut_setup(20));
    CHECK(build_as(1, 2));
    CHECK(build_as(5, 6)); /* a second address space */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 10, 1) == T_OK);
    CHECK(cap_derive(&uts, 10, 11, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive(&uts, 10, 12, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive(&uts, 10, 13, CAP_R_ALL, 0) == T_OK);

    /* RX first: no writable alias anywhere. */
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), RX) == T_OK);
    CHECK(vspace_map(&uts, 5, 11, ua(VA0), RW) == T_EPERM);          /* other AS */
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 0x1000), RW) == T_EPERM); /* same AS */
    CHECK(vspace_map(&uts, 5, 11, ua(VA0), RO) == T_OK);             /* RO alias ok */
    CHECK(vspace_map(&uts, 5, 12, ua(VA0 + 0x1000), RX) == T_OK);    /* RX alias ok */
    CHECK(meta_audit());

    /* Remove every executable mapping; now RW is allowed... */
    CHECK(vspace_unmap(&uts, 10) == T_OK);
    CHECK(vspace_map(&uts, 1, 13, ua(VA0 + 0x2000), RW) == T_EPERM); /* 12 still RX */
    CHECK(vspace_unmap(&uts, 12) == T_OK);
    CHECK(vspace_map(&uts, 1, 13, ua(VA0 + 0x2000), RW) == T_OK);
    CHECK(meta_audit());

    /* ...and RW first blocks RX, in the other order too. */
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), RX) == T_EPERM);
    CHECK(vspace_map(&uts, 5, 12, ua(VA0 + 0x1000), RX) == T_EPERM);
    CHECK(vspace_map(&uts, 5, 12, ua(VA0 + 0x1000), RO) == T_OK);

    /* Deleting a writable alias's capability lifts the block. */
    CHECK(cap_delete(&uts, 13) == T_OK);
    CHECK(meta_audit());
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), RX) == T_OK);

    /* A copy never inherits a mapping (V10), and frames take no badge. */
    CHECK(cap_derive(&uts, 10, 14, CAP_R_ALL, 0) == T_OK);
    CHECK(ut_store[14].map_state == 0 && ut_store[14].map_entry == NULL);
    CHECK(cap_derive(&uts, 10, 15, CAP_R_ALL, 0x99) == T_EINVAL);

    CHECK(vspace_audit(&uts, 1) && vspace_audit(&uts, 5));
    ut_teardown();
}

/* V1-V4, V9, V10: confused deputies and kernel memory. */
static void test_vspace_deputy(void)
{
    CHECK(ut_setup(20));
    CHECK(build_as(1, 2));
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 10, 2) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 2, &uts, 12, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 13, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_PAGE_TABLE, 0, &uts, 14, 1) == T_OK);

    /* Task B holds a copy of the AS capability but no frame. */
    cspace_init(&bcs, b_store, ARRAY_SIZE(b_store));
    CHECK(cap_derive_to(&uts, 1, &bcs, 0, CAP_R_ALL, 0) == T_OK);
    CHECK(vspace_map(&bcs, 0, 10, ua(VA0), RW) == T_EBADCAP);      /* slot 10 is A's, not B's */
    CHECK(vspace_map(&bcs, 0, 1, ua(VA0), RW) == T_EBADCAP);
    /* A delegates read-only; B cannot widen it. */
    CHECK(cap_derive_to(&uts, 10, &bcs, 1, CAP_R_READ | CAP_R_GRANT, 0) == T_OK);
    CHECK(vspace_map(&bcs, 0, 1, ua(VA0), RW) == T_EPERM);
    CHECK(vspace_map(&bcs, 0, 1, ua(VA0), RX) == T_EPERM);
    CHECK(cap_derive(&bcs, 1, 2, CAP_R_ALL, 0) == T_OK);        /* re-derive cannot widen */
    CHECK(vspace_map(&bcs, 0, 2, ua(VA0), RW) == T_EPERM);
    /* An AS capability without WRITE confers no right to map. */
    CHECK(cap_derive_to(&uts, 1, &bcs, 3, CAP_R_READ, 0) == T_OK);
    CHECK(vspace_map(&bcs, 3, 1, ua(VA0), RO) == T_EPERM);
    CHECK(vspace_map(&bcs, 0, 1, ua(VA0), RO) == T_OK);              /* with real authority */

    /* Only Frames can be mapped. */
    CHECK(vspace_map(&uts, 1, 12, ua(VA0 + 0x1000), RO) == T_EBADCAP); /* CNode */
    CHECK(vspace_map(&uts, 1, 13, ua(VA0 + 0x1000), RO) == T_EBADCAP); /* Endpoint */
    CHECK(vspace_map(&uts, 1, 1, ua(VA0 + 0x1000), RO) == T_EBADCAP);  /* AddressSpace */
    CHECK(vspace_map(&uts, 1, 14, ua(VA0 + 0x1000), RO) == T_EBADCAP); /* PageTable */
    CHECK(vspace_map(&uts, 1, 0, ua(VA0 + 0x1000), RO) == T_EBADCAP);  /* Untyped */

    /* Kernel and non-user addresses are unreachable. */
    CHECK(vspace_map(&uts, 1, 11, ua(USER_TOP), RO) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 11, ua(0xFFFF800000000000ull), RO) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 11, ua(0xFFFFFFFF80000000ull), RO) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 8), RO) == T_EINVAL);
    CHECK(vspace_map(&uts, 1, 11, ua(USER_TOP - PAGE_SIZE), RO) == T_ENOENT); /* user, no table */
    CHECK(vspace_install(&uts, 1, 14, ua(0xFFFF800000000000ull), 1) == T_EINVAL);
    CHECK(vspace_install(&uts, 1, 14, ua(0xFFFFFF8000000000ull), 1) == T_EINVAL);

    /* Install rules. */
    CHECK(vspace_install(&uts, 1, 14, ua(VA0), 0) == T_EINVAL);
    CHECK(vspace_install(&uts, 1, 14, ua(VA0), 4) == T_EINVAL);
    CHECK(vspace_install(&uts, 1, 14, ua(VA0 + 0x1000), 3) == T_EINVAL); /* misaligned */
    CHECK(vspace_install(&uts, 1, 14, ua(VA0), 3) == T_EBUSY);           /* entry used */
    CHECK(vspace_install(&uts, 1, 2, ua(2 * VA0), 1) == T_EBUSY);        /* already installed */
    CHECK(vspace_install(&uts, 1, 14, ua(2 * VA0), 3) == T_ENOENT);      /* no PDPT/PD there */
    CHECK(cap_derive(&uts, 14, 15, CAP_R_ALL, 0) == T_EINVAL);       /* not copyable */

    /* One mapping per capability; one frame per entry. */
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 0x1000), RO) == T_OK);
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 0x2000), RO) == T_EBUSY);
    CHECK(vspace_map(&uts, 1, 10, ua(VA0 + 0x1000), RO) == T_EBUSY);

    CHECK(vspace_audit(&uts, 1));
    cspace_destroy(&bcs);
    CHECK(hw_flags(&uts, 1, VA0) == 0); /* B's mapping died with B's cspace */
    CHECK(vspace_audit(&uts, 1));
    ut_teardown();
}

/* V11-V13: teardown in every order leaves nothing dangling. */
static void test_vspace_teardown(void)
{
    uint64_t master = read_cr3() & 0x000FFFFFFFFFF000ull;

    CHECK(ut_setup(20));
    CHECK(build_as(1, 2));
    CHECK(build_as(5, 6));
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 10, 4) == T_OK);
    for (cap_slot_t i = 0; i < 4; i++)
        CHECK(vspace_map(&uts, 1, 10 + i, ua(VA0 + i * PAGE_SIZE), RW) == T_OK);

    /* Frame first: deleting the capability unmaps it, and the TLB no
     * longer translates it. */
    (void)user_read64(1, VA0);         /* warm the TLB */
    CHECK(cap_delete(&uts, 10) == T_OK);
    CHECK(hw_flags(&uts, 1, VA0) == 0);
    CHECK(vspace_audit(&uts, 1));

    /* Deleting a copy removes only the copy's mapping. */
    CHECK(cap_derive(&uts, 11, 20, CAP_R_ALL, 0) == T_OK);
    CHECK(vspace_unmap(&uts, 11) == T_OK);
    CHECK(vspace_map(&uts, 5, 20, ua(VA0), RW) == T_OK);
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 0x1000), RW) == T_OK);
    CHECK(cap_delete(&uts, 20) == T_OK);
    CHECK(hw_flags(&uts, 5, VA0) == 0 && (hw_flags(&uts, 1, VA0 + 0x1000) & PTE_P));

    /* Leaf table first: its frames become unmapped and reusable. */
    CHECK(cap_delete(&uts, 4) == T_OK);
    CHECK(ut_store[11].map_state == 0 && ut_store[11].map_entry == NULL);
    CHECK(ut_store[12].map_state == 0 && ut_store[13].map_state == 0);
    CHECK(hw_flags(&uts, 1, VA0 + 0x1000) == 0);
    CHECK(vspace_audit(&uts, 1));
    CHECK(vspace_map(&uts, 5, 11, ua(VA0 + 0x1000), RW) == T_OK);

    /* Address space first, while active: the CPU falls back to the kernel
     * tables. Only its own entries are detached (V12 as amended, D1): its
     * PDPT is uninstalled but keeps the PD installed in it, so it is
     * inert and cannot be installed until its holder empties it (V15'). */
    CHECK(vspace_activate(&uts, 1) == T_OK);
    CHECK(cap_delete(&uts, 1) == T_OK);
    CHECK(cr3_is_kernel(master));
    CHECK(ut_store[2].map_state == 0 && ut_store[2].map_entry == NULL);
    CHECK((ut_store[3].map_state & 0x8) && ut_store[3].map_entry != NULL); /* untouched */
    CHECK(cap_retype(&uts, 0, CAP_ADDRESS_SPACE, 0, &uts, 1, 1) == T_OK);
    CHECK(vspace_install(&uts, 1, 2, ua(VA0), 1) == T_EBUSY);   /* populated */
    CHECK(vspace_uninstall(&uts, 3) == T_OK);                    /* empty it */
    CHECK(vspace_install(&uts, 1, 2, ua(VA0), 1) == T_OK);
    CHECK(vspace_install(&uts, 1, 3, ua(VA0), 2) == T_OK);
    CHECK(vspace_audit(&uts, 1) && meta_audit());


    /* V13: the TLB never serves a removed translation. Stay inside one
     * activation (no CR3 reload) so only the kernel's own flush can help:
     * read through VA0 to cache it, remap VA0 to another frame, read again. */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 30, 2) == T_OK);
    *(volatile uint64_t *)phys_to_virt(cap_obj_paddr(&ut_store[30])) = 0xAAAA;
    *(volatile uint64_t *)phys_to_virt(cap_obj_paddr(&ut_store[31])) = 0xBBBB;
    CHECK(vspace_map(&uts, 5, 30, ua(VA0 + 0x8000), RO) == T_OK);
    uint64_t before = 0, after = 0;
    int rc_unmap = -1, rc_map = -1;
    if (vspace_activate(&uts, 5) == T_OK) {
        smap_open();
        before = *(volatile uint64_t *)(VA0 + 0x8000);
        smap_close();
        rc_unmap = vspace_unmap(&uts, 30);
        rc_map = vspace_map(&uts, 5, 31, ua(VA0 + 0x8000), RO);
        smap_open();
        after = *(volatile uint64_t *)(VA0 + 0x8000);
        smap_close();
        vspace_activate_kernel();
    }
    CHECK(rc_unmap == T_OK && rc_map == T_OK);
    CHECK(before == 0xAAAA && after == 0xBBBB);

    /* V15': an uninstalled table carries no mappings into another address
     * space. Populate AS 5's tables and pull its PDPT out. Detaching is
     * not destroying: everything below stays mapped and installed, but
     * nothing reaches it any more, and the populated PDPT cannot be
     * installed in AS 1 until its holders have emptied it, level by
     * level. Nothing that was mapped ever becomes visible in AS 1. */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 40, 2) == T_OK);
    CHECK(vspace_map(&uts, 5, 40, ua(VA0 + 0x10000), RW) == T_OK);
    CHECK(vspace_map(&uts, 5, 41, ua(VA0 + 0x11000), RO) == T_OK);
    CHECK(vspace_uninstall(&uts, 6) == T_OK);              /* AS 5's PDPT */
    CHECK(hw_flags(&uts, 5, VA0 + 0x10000) == 0 && hw_flags(&uts, 5, VA0 + 0x11000) == 0);
    CHECK((ut_store[40].map_state & 0x8) && (ut_store[41].map_state & 0x8));   /* still mapped */
    CHECK((ut_store[7].map_state & 0x8) && (ut_store[8].map_state & 0x8));     /* still installed */
    CHECK(vspace_audit(&uts, 5) && meta_audit());
    CHECK(cap_delete(&uts, 2) == T_OK);                    /* free AS 1's slot */
    CHECK(vspace_install(&uts, 1, 6, ua(VA0), 1) == T_EBUSY);   /* inert */
    CHECK(vspace_uninstall(&uts, 7) == T_OK);
    CHECK(vspace_install(&uts, 1, 6, ua(VA0), 1) == T_OK);
    CHECK(vspace_install(&uts, 1, 7, ua(VA0), 2) == T_EBUSY);   /* still holds 8 */
    CHECK(vspace_uninstall(&uts, 8) == T_OK);
    CHECK(vspace_install(&uts, 1, 7, ua(VA0), 2) == T_OK);
    CHECK(vspace_install(&uts, 1, 8, ua(VA0), 3) == T_EBUSY);   /* still maps 40, 41 */
    CHECK(hw_flags(&uts, 1, VA0 + 0x10000) == 0 && hw_flags(&uts, 1, VA0 + 0x11000) == 0);
    CHECK(vspace_unmap(&uts, 40) == T_OK && vspace_unmap(&uts, 41) == T_OK);
    CHECK(vspace_install(&uts, 1, 8, ua(VA0), 3) == T_EBUSY);   /* 11 and 31 too */
    CHECK(vspace_unmap(&uts, 11) == T_OK && vspace_unmap(&uts, 31) == T_OK);
    CHECK(vspace_install(&uts, 1, 8, ua(VA0), 3) == T_OK);
    CHECK(hw_flags(&uts, 1, VA0 + 0x10000) == 0 && hw_flags(&uts, 1, VA0 + 0x11000) == 0);
    CHECK(vspace_audit(&uts, 1) && vspace_audit(&uts, 5) && meta_audit());

    /* Everything at once: revoke the Untyped. */
    CHECK(vspace_activate(&uts, 5) == T_OK);
    CHECK(cap_revoke(&uts, 0, 0) == T_OK);
    CHECK(cr3_is_kernel(master));
    for (cap_slot_t i = 1; i < ARRAY_SIZE(ut_store); i++)
        CHECK(is_free_slot(&uts, i));
    CHECK(cdt_consistent(&uts));
    ut_teardown();
    CHECK(frames_conserved());
}

extern const char __text_start[], __text_end[];
extern const char __rodata_start[], __rodata_end[];
extern const char __data_start[], __data_end[];

static bool is_stack_guard(uintptr_t va)
{
    for (size_t i = 0; i < kstack_count; i++)
        if (va == (uintptr_t)kstacks[i].guard)
            return true;
    return false;
}

/* Every page in [start, end) must have exactly the expected W and NX bits;
 * the only unmapped pages allowed are the stack guards. */
static bool range_has(const char *start, const char *end, bool want_w, bool want_nx)
{
    for (uintptr_t va = ALIGN_DOWN((uintptr_t)start, PAGE_SIZE); va < (uintptr_t)end; va += PAGE_SIZE) {
        uint64_t f;
        if (paging_query(va, &f) != 0) {
            if (is_stack_guard(va))
                continue;
            return false;
        }
        if (!!(f & PTE_W) != want_w || !!(f & PTE_NX) != want_nx || (f & PTE_U))
            return false;
    }
    return true;
}

static void test_wx(void)
{
    CHECK(range_has(__text_start, __text_end, false, false));   /* R-X */
    CHECK(range_has(__rodata_start, __rodata_end, false, true)); /* R-- */
    CHECK(range_has(__data_start, __data_end, true, true));      /* RW- */
}

static void test_stack_guards(void)
{
    extern char boot_stack[], boot_stack_top[];
    uint64_t f;
    char probe;

    CHECK(kstack_count == 5);
    for (size_t i = 0; i < kstack_count; i++) {
        CHECK(paging_query((uintptr_t)kstacks[i].guard, &f) == -1);
        CHECK(kstacks[i].base == kstacks[i].guard + PAGE_SIZE);
        CHECK((uintptr_t)kstacks[i].top % 16 == 0);
        CHECK(kstacks[i].budget <= (size_t)(kstacks[i].top - kstacks[i].base) / 2);
    }
    /* We are running on the guarded boot stack. */
    CHECK(&probe > boot_stack && &probe < boot_stack_top);
}

/* Per-CPU data and extended state groundwork (ADR 0008, T6-T9). */
static uint8_t xarea_a[XSAVE_AREA_MAX] __aligned(64);
static uint8_t xarea_b[XSAVE_AREA_MAX] __aligned(64);

static void test_percpu_fpu(void)
{
    extern char kstack_cpu0_top[];
    struct percpu *p = this_cpu();

    CHECK(p != NULL && p->self == p);
    CHECK(rdmsr(MSR_GS_BASE) == (uint64_t)p);
    CHECK(rdmsr(MSR_KERNEL_GS_BASE) == 0);     /* user GS base starts at 0 */
    CHECK(p->kstack_top == (uint64_t)kstack_cpu0_top);
    CHECK(p->kstack_top % 16 == 0);

    CHECK(!(read_cr0() & (CR0_EM | CR0_TS)));   /* no lazy FPU switching */
    CHECK(read_cr4() & CR4_OSXSAVE);
    CHECK(xsave_size > 512 && xsave_size <= XSAVE_AREA_MAX);
    CHECK((xcr0 & 3) == 3);                     /* x87 and SSE always */

    /* T7: a fresh area loads the architectural initial state. Dirty the
     * live state first, then restore a fresh area and save it back. */
    memset(xarea_b, 0x5A, sizeof(xarea_b));
    fpu_init_area(xarea_a);
    fpu_restore(xarea_a);
    fpu_save(xarea_b);
    CHECK(*(uint16_t *)(xarea_b + 0) == 0x037F);         /* x87 FCW init */
    CHECK(*(uint32_t *)(xarea_b + 24) == MXCSR_DEFAULT);  /* MXCSR */
    bool xmm_zero = true;
    for (size_t i = 160; i < 160 + 16 * 16; i++)          /* XMM0-15 */
        xmm_zero &= xarea_b[i] == 0;
    CHECK(xmm_zero);
}

/* ---- Threads (docs/adr/0008-threads-user-entry-syscalls.md) ---- */

static bool regs_equal(const struct user_regs *a, const struct user_regs *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* T6, T7, T11-T15: a new thread's state, and hostile register values. */
static void test_thread_regs(void)
{
    struct user_regs r, before;
    uint32_t st;

    CHECK(ut_setup(18));
    CHECK(cap_retype(&uts, 0, CAP_THREAD, 0, &uts, 1, 1) == T_OK);
    struct thread *t = thread_of(&ut_store[1]);

    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_INACTIVE);
    CHECK(thread_read_regs(&uts, 1, &r) == T_OK);
    static const struct user_regs zero;
    CHECK(regs_equal(&r, &zero));                               /* T7 */
    CHECK(*(uint32_t *)((uint8_t *)thread_xsave_area(t) + 24) == MXCSR_DEFAULT);
    for (unsigned i = 0; i < TS_COUNT; i++)
        CHECK(t->bind[i].slot_flags == (SLOT_BINDING | i) && t->bind[i].type == CAP_NULL);

    r.rip = 0x400000;
    r.rsp = 0x7FFFFFFFF000ull;
    r.rflags = RFLAGS_USER_MASK;
    r.rax = 1; r.r15 = 15;
    r.fs_base = 0x1000; r.gs_base = USER_TOP - 1;
    CHECK(thread_write_regs(&uts, 1, &r) == T_OK);
    CHECK(thread_read_regs(&uts, 1, &before) == T_OK && regs_equal(&r, &before));

    /* Every hostile value is refused and changes nothing. */
    static const struct { int field; uint64_t v; } bad[] = {
        { 0, USER_TOP }, { 0, 0xFFFF800000000000ull }, { 0, 0xFFFFFFFF80000000ull },
        { 0, 0x0000800000000001ull }, { 0, 0x8000000000000000ull }, { 0, ~0ull },
        { 1, USER_TOP }, { 1, 0xFFFFFFFF80001000ull }, { 1, 0x00FF000000000000ull },
        { 2, 1u << 12 },  /* IOPL */ { 2, 3u << 12 }, { 2, 1u << 8 },  /* TF */
        { 2, 1u << 14 },  /* NT */   { 2, 1u << 16 }, /* RF */ { 2, 1u << 17 }, /* VM */
        { 2, 1u << 19 },  /* VIF */  { 2, 1u << 20 }, /* VIP */ { 2, RFLAGS_IF },
        { 2, 1ull << 40 },
        { 3, USER_TOP }, { 3, 0xFFFF800000000000ull },
        { 4, 0x8000000000000000ull }, { 4, 0xFFFFFFFFFFFFF000ull },
    };
    for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
        struct user_regs h = r;
        switch (bad[i].field) {
        case 0: h.rip = bad[i].v; break;
        case 1: h.rsp = bad[i].v; break;
        case 2: h.rflags = bad[i].v; break;
        case 3: h.fs_base = bad[i].v; break;
        default: h.gs_base = bad[i].v; break;
        }
        CHECK(thread_write_regs(&uts, 1, &h) == T_EINVAL);
    }
    CHECK(thread_read_regs(&uts, 1, &r) == T_OK && regs_equal(&r, &before));

    ut_teardown();
}

/* T1-T5: authority, binding, runnability, and losing bindings. */
static void test_thread_lifecycle(void)
{
    struct user_regs r;
    uint32_t st;

    CHECK(ut_setup(18));
    CHECK(cap_retype(&uts, 0, CAP_THREAD, 0, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_ADDRESS_SPACE, 0, &uts, 2, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 4, &uts, 3, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 4, 1) == T_OK);
    struct thread *t = thread_of(&ut_store[1]);

    /* T4: no bindings, not runnable. */
    CHECK(thread_resume(&uts, 1) == T_EPERM);

    /* T1: binding needs WRITE on the thread, GRANT on the source, and the
     * right type for the slot. */
    CHECK(cap_derive(&uts, 2, 10, CAP_R_READ | CAP_R_WRITE, 0) == T_OK); /* no GRANT */
    CHECK(thread_bind(&uts, 1, TS_ASPACE, &uts, 10) == T_EPERM);
    CHECK(thread_bind(&uts, 1, TS_ASPACE, &uts, 3) == T_EBADCAP);         /* CNode */
    CHECK(thread_bind(&uts, 1, TS_COUNT, &uts, 2) == T_EINVAL);
    CHECK(thread_bind(&uts, 1, TS_ASPACE, &uts, 2) == T_OK);
    CHECK(thread_bind(&uts, 1, TS_ASPACE, &uts, 2) == T_EBUSY);
    CHECK(thread_resume(&uts, 1) == T_EPERM);                              /* no cspace yet */
    CHECK(thread_bind(&uts, 1, TS_CSPACE, &uts, 3) == T_OK);
    CHECK(thread_bind(&uts, 1, TS_IPCBUF, &uts, 4) == T_OK);

    /* T3: another task's view. */
    cspace_init(&bcs, b_store, ARRAY_SIZE(b_store));
    CHECK(cap_derive_to(&uts, 1, &bcs, 0, CAP_R_READ, 0) == T_OK);   /* read-only */
    CHECK(cap_derive_to(&uts, 1, &bcs, 1, CAP_R_GRANT, 0) == T_OK);  /* neither */
    CHECK(thread_read_regs(&bcs, 0, &r) == T_OK);
    CHECK(thread_write_regs(&bcs, 0, &r) == T_EPERM);
    CHECK(thread_resume(&bcs, 0) == T_EPERM);
    CHECK(thread_suspend(&bcs, 0) == T_EPERM);
    CHECK(thread_bind(&bcs, 0, TS_FAULT, &uts, 4) == T_EPERM);
    CHECK(thread_unbind(&bcs, 0, TS_ASPACE) == T_EPERM);
    CHECK(thread_read_regs(&bcs, 1, &r) == T_EPERM);
    CHECK(thread_get_state(&bcs, 1, &st) == T_EPERM);
    CHECK(thread_read_regs(&bcs, 5, &r) == T_EBADCAP);                 /* not held */
    cspace_destroy(&bcs);

    /* Resume, suspend, resume. */
    CHECK(thread_resume(&uts, 1) == T_OK);
    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_RUNNABLE && !runq_empty());
    CHECK(thread_resume(&uts, 1) == T_EBUSY);
    CHECK(thread_suspend(&uts, 1) == T_OK);
    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_INACTIVE && runq_empty());
    CHECK(thread_resume(&uts, 1) == T_OK);

    /* T2: deleting the AS's original capability leaves the binding (a
     * copy) and the object alive; revoking it removes the binding, and
     * the thread stops before the deletion completes. */
    CHECK(cap_delete(&uts, 10) == T_OK);
    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_RUNNABLE);
    CHECK(cap_revoke(&uts, 2, 0) == T_OK);
    CHECK(t->bind[TS_ASPACE].type == CAP_NULL);
    CHECK(t->bind[TS_ASPACE].slot_flags == (SLOT_BINDING | TS_ASPACE)); /* slot stays a binding slot */
    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_INACTIVE && runq_empty());
    CHECK(thread_resume(&uts, 1) == T_EPERM);

    /* Losing an optional binding does not stop the thread... */
    CHECK(thread_bind(&uts, 1, TS_ASPACE, &uts, 2) == T_OK);
    CHECK(thread_resume(&uts, 1) == T_OK);
    CHECK(cap_revoke(&uts, 4, 0) == T_OK);                  /* IPC buffer */
    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_RUNNABLE);
    /* ...losing the cspace does, by unbind or by destroying the CNode. */
    CHECK(thread_unbind(&uts, 1, TS_CSPACE) == T_OK);
    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_INACTIVE && runq_empty());
    CHECK(thread_bind(&uts, 1, TS_CSPACE, &uts, 3) == T_OK);
    CHECK(thread_resume(&uts, 1) == T_OK);
    CHECK(cap_revoke(&uts, 3, 0) == T_OK);
    CHECK(thread_get_state(&uts, 1, &st) == T_OK && st == THREAD_INACTIVE && runq_empty());

    /* T5: destroying a runnable thread leaves no queue entry and no
     * binding copies behind. */
    CHECK(thread_bind(&uts, 1, TS_CSPACE, &uts, 3) == T_OK);
    CHECK(thread_resume(&uts, 1) == T_OK);
    CHECK(cap_delete(&uts, 1) == T_OK);
    CHECK(runq_empty());
    CHECK(!cap_has_children(&ut_store[2]) && !cap_has_children(&ut_store[3]));
    CHECK(cdt_consistent(&uts));

    ut_teardown();
    CHECK(runq_empty());
}

/* ---- Ring 3 (ADR 0008) ---- */

#define UCODE_VA   0x400000ull
#define UDATA_VA   0x401000ull
#define USTACK_TOP 0x403000ull

#define PROGS(X) X(sys1) X(block) X(spin) X(runner) X(hello) X(fresh) X(fpu_a) X(fpu_b) X(preserve) X(ac) X(segbase) X(badsys) \
    X(f_ud) X(f_bp) X(f_de) X(f_pf_read) X(f_pf_write) X(f_pf_exec) X(f_gp_hlt) X(f_gp_io) \
    X(f_gp_int) X(f_gp_noncanon) X(f_ss) X(f_xm) X(f_ac) X(sysret_edge) \
    X(f_ud_go) X(f_pf_retry) X(f_pf_kernel) X(resend)
#define DECL(n) extern const char prog_##n##_start[], prog_##n##_end[];
PROGS(DECL)
#define P(n) prog_##n##_start, prog_##n##_end

/* A complete task in uts slots b..b+8: AS, three page tables, code / data /
 * stack frames, a cspace and a thread, made runnable. With `at_top`, the
 * program is placed so that it ends exactly at USER_TOP. */
enum { T_AS, T_PT, T_CODE = 4, T_DATA, T_STACK, T_CS, T_THR, T_SLOTS };

static bool spawn(cap_slot_t b, const char *start, const char *end, bool at_top,
                  struct user_regs *init)
{
    uint64_t region = at_top ? USER_TOP - (2ull << 20) : UCODE_VA;
    uint64_t code_va = at_top ? USER_TOP - PAGE_SIZE : UCODE_VA;
    size_t len = (size_t)(end - start);

    if (len > PAGE_SIZE ||
        cap_retype(&uts, 0, CAP_ADDRESS_SPACE, 0, &uts, b + T_AS, 1) != T_OK ||
        cap_retype(&uts, 0, CAP_PAGE_TABLE, 0, &uts, b + T_PT, 3) != T_OK ||
        cap_retype(&uts, 0, CAP_FRAME, 0, &uts, b + T_CODE, 3) != T_OK ||
        cap_retype(&uts, 0, CAP_CNODE, 4, &uts, b + T_CS, 1) != T_OK ||
        cap_retype(&uts, 0, CAP_THREAD, 0, &uts, b + T_THR, 1) != T_OK)
        return false;
    if (vspace_install(&uts, b + T_AS, b + T_PT, ua(ALIGN_DOWN(region, 1ull << 39)), 1) ||
        vspace_install(&uts, b + T_AS, b + T_PT + 1, ua(ALIGN_DOWN(region, 1ull << 30)), 2) ||
        vspace_install(&uts, b + T_AS, b + T_PT + 2, ua(region), 3))
        return false;

    uint8_t *code = phys_to_virt(cap_obj_paddr(&ut_store[b + T_CODE]));
    memcpy(code + (at_top ? PAGE_SIZE - len : 0), start, len);
    if (vspace_map(&uts, b + T_AS, b + T_CODE, ua(code_va), RX) ||
        (!at_top && (vspace_map(&uts, b + T_AS, b + T_DATA, ua(UDATA_VA), RW) ||
                     vspace_map(&uts, b + T_AS, b + T_STACK, ua(USTACK_TOP - PAGE_SIZE), RW))))
        return false;

    if (thread_bind(&uts, b + T_THR, TS_ASPACE, &uts, b + T_AS) ||
        thread_bind(&uts, b + T_THR, TS_CSPACE, &uts, b + T_CS))
        return false;
    struct user_regs r = init ? *init : (struct user_regs){ 0 };
    r.rip = at_top ? USER_TOP - len : code_va;
    r.rsp = USTACK_TOP;
    return thread_write_regs(&uts, b + T_THR, &r) == T_OK &&
           thread_resume(&uts, b + T_THR) == T_OK;
}

static uint32_t tstate(cap_slot_t b)
{
    uint32_t st;
    return thread_get_state(&uts, b + T_THR, &st) == T_OK ? st : 0xFFFFFFFF;
}

static struct fault_record tfault(cap_slot_t b)
{
    struct fault_record f;
    if (thread_read_fault(&uts, b + T_THR, &f) != T_OK)
        f = (struct fault_record){ .vector = 0xFFFF };
    return f;
}

static void run_user(void)
{
    test_report_count = 0;
    sched_run();
}

static void test_user_basics(void)
{
    uint64_t viol = entry_obs.violations;

    /* Privilege and flags from ring 3. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(hello), false, NULL));
    run_user();
    CHECK(test_report_count == 1);
    CHECK(test_reports[0].a == (GDT_UCODE | 3) && test_reports[0].b == (GDT_UDATA | 3));
    CHECK((test_reports[0].c & RFLAGS_IF) && !(test_reports[0].c & (3u << 12)));
    CHECK(test_reports[0].d == 0x7a72);
    CHECK(tstate(10) == THREAD_INACTIVE);       /* exited */
    CHECK(cr3_is_kernel(read_cr3() & 0x000FFFFFFFFFF000ull) && this_cpu()->current == NULL);
    ut_teardown();

    /* T7, T28: a new thread finds nothing of anyone else's. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(fresh), false, NULL));
    run_user();
    CHECK(test_report_count == 1);
    CHECK(test_reports[0].a == 0 && test_reports[0].b == 0);
    CHECK(test_reports[0].c == 0x037F && test_reports[0].d == MXCSR_DEFAULT);
    ut_teardown();

    /* T6: vector and x87 state across a switch, in both directions. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(fpu_a), false, NULL));
    CHECK(spawn(20, P(fpu_b), false, NULL));
    run_user();
    CHECK(test_report_count == 2);
    CHECK(test_reports[0].d == 2 && test_reports[0].a == 0 && test_reports[0].b == 0);
    CHECK(((test_reports[0].c >> 11) & 7) == 0);
    CHECK(test_reports[1].d == 1 && test_reports[1].a == 0x1122334455667788ull &&
          test_reports[1].b == 0x1122334455667788ull);
    CHECK(test_reports[1].c == 0x400921FB54442D18ull); /* pi */
    ut_teardown();

    /* T20, T18: 2000 round trips preserve every register, via SYSRET. */
    uint64_t sysrets = entry_obs.sysret_returns;
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(preserve), false, NULL));
    run_user();
    CHECK(test_report_count == 1 && test_reports[0].a == 0 && test_reports[0].b == 2000);
    CHECK(entry_obs.sysret_returns - sysrets >= 2000);
    ut_teardown();

    /* T31: unknown syscall numbers are errors, not faults. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(badsys), false, NULL));
    run_user();
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_EINVAL);
    CHECK(tstate(10) == THREAD_INACTIVE);
    ut_teardown();

    CHECK(entry_obs.violations == viol);
}

/* T9, T15, T32: AC never reaches the kernel; FS/GS bases are the thread's. */
static void test_user_entry_state(void)
{
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(ac), false, NULL));
    run_user();
    CHECK(test_report_count == 1 && test_reports[0].a == 0xac);
    CHECK(test_reports[0].user_rflags & (1u << 18));          /* user had AC */
    CHECK(!(test_reports[0].kernel_rflags & (1u << 18)));     /* kernel did not */
    CHECK(tstate(10) == THREAD_FAULTED && tfault(10).vector == 6);
    CHECK(entry_obs.last_user_rflags & (1u << 18));           /* exception path too */
    CHECK(!(entry_obs.last_rflags & (1u << 18)) && !(entry_obs.last_rflags & (1u << 10)));
    ut_teardown();

    CHECK(ut_setup(20));
    struct user_regs r = { .fs_base = UDATA_VA, .gs_base = UDATA_VA + 8 };
    CHECK(spawn(10, P(segbase), false, &r));
    uint64_t *data = phys_to_virt(cap_obj_paddr(&ut_store[10 + T_DATA]));
    data[0] = 0xF5F5F5F5ull;
    data[1] = 0x65656565ull;
    run_user();
    CHECK(test_report_count == 1);
    CHECK(test_reports[0].a == 0xF5F5F5F5ull && test_reports[0].b == 0x65656565ull);
    CHECK(rdmsr(MSR_GS_BASE) == (uint64_t)this_cpu());        /* kernel GS restored */
    ut_teardown();
}

/* T29: every user fault faults the thread, never the kernel. */
static void test_user_faults(void)
{
    /* `alt` is what QEMU's software emulator (TCG) produces where it
     * deviates from hardware: page faults for non-canonical accesses, and no
     * #XM or #AC at all (the programs then reach their trailing ud2). The
     * thread is faulted and the kernel survives either way; the hardware
     * vectors still need confirming on real hardware or KVM. */
    static const struct {
        const char *s, *e;
        uint64_t vector, alt;
    } cases[] = {
        { P(f_ud), 6, 6 },  { P(f_bp), 3, 3 },  { P(f_de), 0, 0 },  { P(f_pf_read), 14, 14 },
        { P(f_pf_write), 14, 14 }, { P(f_pf_exec), 14, 14 }, { P(f_gp_hlt), 13, 13 },
        { P(f_gp_io), 13, 13 }, { P(f_gp_int), 13, 13 }, { P(f_gp_noncanon), 13, 14 },
        { P(f_ss), 12, 14 }, { P(f_xm), 19, 6 }, { P(f_ac), 17, 6 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        CHECK(ut_setup(20));
        CHECK(spawn(10, cases[i].s, cases[i].e, false, NULL));
        run_user();
        struct fault_record f = tfault(10);
        CHECK(tstate(10) == THREAD_FAULTED);
        CHECK(f.vector == cases[i].vector || f.vector == cases[i].alt);
        if (f.vector != cases[i].vector)
            kprintf("    note: fault case %zu raised vector %lu (hardware: %lu)\n", i, f.vector,
                    cases[i].vector);
        if (cases[i].s == prog_f_pf_read_start)
            CHECK(f.addr == 0x10 && (f.error & 7) == 4);        /* user, read, not present */
        if (cases[i].s == prog_f_pf_write_start)
            CHECK(f.addr >= UCODE_VA && f.addr < UCODE_VA + PAGE_SIZE && (f.error & 7) == 7);
        if (cases[i].s == prog_f_pf_exec_start)
            CHECK(f.addr == UDATA_VA && (f.error & 16) && (f.error & 5) == 5);
        ut_teardown();
    }

    /* Two faulting threads and a healthy one: the healthy one still runs. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(f_ud), false, NULL));
    CHECK(spawn(20, P(hello), false, NULL));
    CHECK(spawn(30, P(f_gp_hlt), false, NULL));
    run_user();
    CHECK(tstate(10) == THREAD_FAULTED && tstate(30) == THREAD_FAULTED);
    CHECK(tstate(20) == THREAD_INACTIVE && test_report_count == 1);
    ut_teardown();
}

/* T17, T18: a syscall in the last two bytes of user space. Its return
 * address is USER_TOP, which is non-canonical: SYSRET there would fault in
 * ring 0 on the user's stack (CVE-2012-0217). The kernel must neither
 * SYSRET nor IRETQ to it; the thread is faulted instead. */
static void test_user_sysret_edge(void)
{
    uint64_t sysrets = entry_obs.sysret_returns, irets = entry_obs.iret_returns;
    struct user_regs r = { .rax = SYS_TEST_REPORT, .rdi = 0xED6E };

    CHECK(ut_setup(20));
    CHECK(spawn(10, P(sysret_edge), true, &r));
    run_user();
    CHECK(test_report_count == 1 && test_reports[0].a == 0xED6E); /* the syscall ran */
    CHECK(tstate(10) == THREAD_FAULTED && tfault(10).vector == VEC_BAD_STATE);
    CHECK(tfault(10).rip == USER_TOP);
    CHECK(entry_obs.sysret_returns == sysrets);           /* never SYSRET */
    CHECK(entry_obs.iret_returns == irets + 1);           /* only the initial entry */
    ut_teardown();
}

/* T25-T27, T35: the copy routines. */
static uint8_t kbuf[COPY_MAX + 16];

static bool all_bytes(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return false;
    return true;
}

static void test_uaccess(void)
{
    extern const char copy_user_insn[];
    uint64_t rflags;

    CHECK(ut_setup(20));
    CHECK(spawn(10, P(hello), false, NULL));   /* code RX, data + stack RW */
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    uint8_t *data = phys_to_virt(cap_obj_paddr(&ut_store[10 + T_DATA]));
    uint8_t *stack = phys_to_virt(cap_obj_paddr(&ut_store[10 + T_STACK]));
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        data[i] = (uint8_t)i;
        stack[i] = (uint8_t)(i ^ 0x5A);
    }

    CHECK(vspace_activate(&uts, 10 + T_AS) == T_OK);
    uint64_t faults = entry_obs.copy_faults;

    /* Success: a whole page, and a range crossing into the next page. */
    CHECK(copy_from_user(kbuf, ua(UDATA_VA), PAGE_SIZE) == T_OK && memcmp(kbuf, data, PAGE_SIZE) == 0);
    CHECK(copy_from_user(kbuf, ua(UDATA_VA + 0x800), PAGE_SIZE) == T_OK);
    CHECK(memcmp(kbuf, data + 0x800, 0x800) == 0 && memcmp(kbuf + 0x800, stack, 0x800) == 0);

    /* Faults: first byte, last byte, partway. Destination always zeroed. */
    memset(kbuf, 0xEE, sizeof(kbuf));
    CHECK(copy_from_user(kbuf, ua(USTACK_TOP), 16) == T_EFAULT && all_bytes(kbuf, 16, 0));
    memset(kbuf, 0xEE, sizeof(kbuf));
    CHECK(copy_from_user(kbuf, ua(USTACK_TOP - 15), 16) == T_EFAULT && all_bytes(kbuf, 16, 0));
    memset(kbuf, 0xEE, sizeof(kbuf));
    CHECK(copy_from_user(kbuf, ua(USTACK_TOP - 0x800), PAGE_SIZE) == T_EFAULT);
    CHECK(all_bytes(kbuf, PAGE_SIZE, 0) && kbuf[PAGE_SIZE] == 0xEE); /* zeroed, not beyond */
    CHECK(entry_obs.copy_faults == faults + 3);                      /* real faults, recovered */

    /* Rejected by the range check, before any access. */
    faults = entry_obs.copy_faults;
    CHECK(copy_from_user(kbuf, ua(0xFFFF800000000000ull), 8) == T_EFAULT);
    CHECK(copy_from_user(kbuf, ua(0xFFFFFFFF80000000ull), 8) == T_EFAULT);
    CHECK(copy_from_user(kbuf, ua(~0ull - 7), 16) == T_EFAULT);      /* wraps */
    CHECK(copy_from_user(kbuf, ua(USER_TOP - 7), 8) == T_EFAULT);    /* one byte over */
    CHECK(copy_from_user(kbuf, ua(UDATA_VA), COPY_MAX + 1) == T_EINVAL);
    CHECK(copy_from_user(kbuf, ua(0xFFFF800000000000ull), 0) == T_EFAULT);
    CHECK(entry_obs.copy_faults == faults);
    CHECK(copy_from_user(kbuf, ua(UDATA_VA), 0) == T_OK);
    /* Exactly at USER_TOP: in range, unmapped, so a real recovered fault. */
    CHECK(copy_from_user(kbuf, ua(USER_TOP - 8), 8) == T_EFAULT);
    CHECK(entry_obs.copy_faults == faults + 1);

    /* copy_to_user: writable page, and a read-only (code) page. */
    memset(kbuf, 0x77, 64);
    CHECK(copy_to_user(ua(UDATA_VA + 64), kbuf, 64) == T_OK && all_bytes(data + 64, 64, 0x77));
    CHECK(copy_to_user(ua(UCODE_VA), kbuf, 8) == T_EFAULT);
    CHECK(copy_to_user(ua(0xFFFF800000001000ull), kbuf, 8) == T_EFAULT);

    /* SMAP is closed again after every path. */
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    CHECK(!(rflags & (1u << 18)) && (read_cr4() & CR4_SMAP));

    /* The recovery decision itself. */
    CHECK(copy_fault_recoverable((uint64_t)copy_user_insn, UDATA_VA));
    CHECK(copy_fault_recoverable((uint64_t)copy_user_insn, USER_TOP - 1));
    CHECK(!copy_fault_recoverable((uint64_t)copy_user_insn, USER_TOP));
    CHECK(!copy_fault_recoverable((uint64_t)copy_user_insn, 0xFFFF800000000000ull));
    CHECK(!copy_fault_recoverable((uint64_t)copy_user_insn + 2, UDATA_VA));
    CHECK(!copy_fault_recoverable((uint64_t)copy_from_user, UDATA_VA));

    vspace_activate_kernel();
    ut_teardown();
}

/* ---- Syscalls from ring 3 (T20-T24, T27) ---- */

struct sysreq { uint64_t nr, a[6], result; };
#define REQ_SENTINEL 0x5E5E5E5E5E5E5E5Eull
#define UBUF       0x402000ull   /* scratch buffers in the stack page */

static struct sysreq *reqs(cap_slot_t b)
{
    return phys_to_virt(cap_obj_paddr(&ut_store[b + T_DATA]));
}

static void *ubuf(cap_slot_t b, uint64_t va)
{
    return (uint8_t *)phys_to_virt(cap_obj_paddr(&ut_store[b + T_STACK])) + (va - UBUF);
}

static unsigned nreq;
static void req(cap_slot_t b, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4)
{
    struct sysreq *r = &reqs(b)[nreq++];
    *r = (struct sysreq){ nr, { a0, a1, a2, a3, a4, 0 }, REQ_SENTINEL };
    reqs(b)[nreq].nr = (uint64_t)-1;
}
#define REQ(b, nr, ...) R6(b, nr, ##__VA_ARGS__, 0, 0, 0, 0, 0)
#define R6(b, nr, a0, a1, a2, a3, a4, ...) req(b, nr, a0, a1, a2, a3, a4)

static int64_t res(cap_slot_t b, unsigned i) { return (int64_t)reqs(b)[i].result; }

static struct cspace task_cs(cap_slot_t b)
{
    struct cspace cs = { 0 };
    CHECK(cnode_view(&uts, b + T_CS, &cs) == T_OK);
    return cs;
}

/* T27: rewrite the user's register block right after the kernel copies
 * it, with a hostile RIP. */
static void hostile_rewrite(uaddr_t src, size_t len)
{
    if (len == sizeof(struct user_regs))
        ((struct user_regs *)ubuf(10, ua_raw(src)))->rip = 0xFFFFFFFF80000000ull;
}

static void test_syscalls(void)
{
    const char msg[] = "hello from ring 3\n";
    const uint64_t MSG = UBUF + 0x100, INFO = UBUF + 0x200, REGS_OK = UBUF + 0x300,
                   REGS_BAD = UBUF + 0x400, REGS_OUT = UBUF + 0x500;

    CHECK(ut_setup(20));
    CHECK(spawn(10, P(runner), false, NULL));
    struct cspace cs = task_cs(10);

    /* The task's capabilities: what it may do, and nothing else (T23). */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &cs, 0, 1) == T_OK);       /* 0: Untyped */
    CHECK(cap_derive_to(&uts, 10 + T_AS, &cs, 1, CAP_R_ALL, 0) == T_OK);    /* 1: own AS */
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &cs, 2, 1) == T_OK);       /* 2: endpoint */
    CHECK(cap_install(&cs, 3, CAP_DEBUG_CONSOLE, CAP_R_WRITE, 0) == T_OK); /* 3: console */
    CHECK(cap_derive_to(&uts, 10 + T_DATA, &cs, 4, CAP_R_READ | CAP_R_GRANT, 0) == T_OK); /* 4 */
    CHECK(cap_derive_to(&uts, 10 + T_CS, &cs, 10, CAP_R_ALL, 0) == T_OK);   /* 10: own cspace */

    /* A second program in the code page, for a thread the task creates. */
    memcpy((uint8_t *)phys_to_virt(cap_obj_paddr(&ut_store[10 + T_CODE])) + 0x800,
           prog_hello_start, (size_t)(prog_hello_end - prog_hello_start));
    memcpy(ubuf(10, MSG), msg, sizeof(msg) - 1);
    *(struct user_regs *)ubuf(10, REGS_OK) = (struct user_regs){ .rip = UCODE_VA + 0x800,
                                                                  .rsp = UBUF + 0xF00 };
    *(struct user_regs *)ubuf(10, REGS_BAD) = (struct user_regs){ .rip = 0xFFFF800000000000ull };

    nreq = 0;
    REQ(10, SYS_CAP_IDENTIFY, 2, INFO);                      /* 0 */
    REQ(10, SYS_CAP_IDENTIFY, 9, INFO);                      /* 1: empty */
    REQ(10, SYS_CAP_IDENTIFY, 99, INFO);                     /* 2: out of range */
    REQ(10, SYS_CAP_IDENTIFY, 0x100000002ull, INFO);         /* 3: would alias slot 2 */
    REQ(10, SYS_CAP_IDENTIFY, 2, 0xFFFF800000000000ull);     /* 4: kernel out-pointer */
    REQ(10, SYS_CAP_IDENTIFY, 2, UCODE_VA);                  /* 5: read-only out-pointer */
    REQ(10, SYS_CAP_DERIVE, 2, 5, CAP_R_READ, 0);            /* 6 */
    REQ(10, SYS_CAP_DERIVE, 0, 6, CAP_R_ALL, 0);             /* 7: Untyped not copyable */
    REQ(10, SYS_RETYPE, 0, CAP_FRAME, 0, 7, 1);              /* 8 */
    REQ(10, SYS_VS_MAP, 1, 7, 0x404000, RW);                 /* 9 */
    REQ(10, SYS_VS_MAP, 1, 4, 0x405000, RW);                 /* 10: read-only cap */
    REQ(10, SYS_VS_MAP, 1, 4, 0x405000, RO);                 /* 11 */
    REQ(10, SYS_VS_MAP, 1, 7, 0xFFFF800000000000ull, RO);    /* 12: kernel address */
    REQ(10, SYS_RETYPE, 0, CAP_THREAD, 0, 8, 1);             /* 13 */
    REQ(10, SYS_TH_BIND, 8, TS_ASPACE, 1);                   /* 14 */
    REQ(10, SYS_TH_BIND, 8, TS_CSPACE, 10);                  /* 15 */
    REQ(10, SYS_TH_WRITE_REGS, 8, REGS_OK);                  /* 16: rewritten after copy */
    REQ(10, SYS_TH_WRITE_REGS, 8, REGS_BAD);                 /* 17 */
    REQ(10, SYS_TH_RESUME, 8);                               /* 18 */
    REQ(10, SYS_TH_READ_REGS, 8, REGS_OUT);                  /* 19 */
    REQ(10, SYS_DEBUG_WRITE, 3, MSG, sizeof(msg) - 1);       /* 20 */
    REQ(10, SYS_DEBUG_WRITE, 2, MSG, sizeof(msg) - 1);       /* 21: not a console */
    REQ(10, SYS_DEBUG_WRITE, 3, MSG, DEBUG_WRITE_MAX + 1);   /* 22 */
    REQ(10, SYS_VS_UNMAP, 7);                                /* 23 */
    REQ(10, SYS_CAP_DELETE, 5);                              /* 24 */
    REQ(10, 0x7777);                                         /* 25 */
    REQ(10, SYS_CAP_REVOKE, 2, 0x100000000ull);              /* 26: the old budget
                                                              * argument is ignored (E6) */
    REQ(10, SYS_RETYPE, 0, 0x100000000ull + CAP_FRAME, 0, 9, 1); /* 27: type > 32 bits */

    uaccess_test_hook = hostile_rewrite;
    run_user();
    uaccess_test_hook = NULL;

    static const int64_t want[] = {
        T_OK, T_EBADCAP, T_ERANGE, T_ERANGE, T_EFAULT, T_EFAULT, T_OK, T_EINVAL,
        T_OK, T_OK, T_EPERM, T_OK, T_EINVAL, T_OK, T_OK, T_OK,
        T_OK, T_EINVAL, T_OK, T_OK, sizeof(msg) - 1, T_EBADCAP, T_EINVAL, T_OK,
        T_OK, T_EINVAL, T_OK, T_EINVAL,
    };
    CHECK(nreq == ARRAY_SIZE(want));
    for (unsigned i = 0; i < ARRAY_SIZE(want); i++) {
        CHECK(res(10, i) == want[i]);
        if (res(10, i) != want[i])
            kprintf("    request %u: got %ld, want %ld\n", i, res(10, i), want[i]);
    }

    struct cap_info *info = ubuf(10, INFO);
    CHECK(info->type == CAP_ENDPOINT && info->rights == CAP_R_ALL);
    /* T27: the kernel used its copy; the user buffer was rewritten after. */
    CHECK(((struct user_regs *)ubuf(10, REGS_OK))->rip == 0xFFFFFFFF80000000ull);
    CHECK(((struct user_regs *)ubuf(10, REGS_OUT))->rip == UCODE_VA + 0x800);
    /* The thread the task created, from its own Untyped, ran in ring 3. */
    CHECK(test_report_count == 1 && test_reports[0].a == (GDT_UCODE | 3));
    CHECK(tstate(10) == THREAD_INACTIVE);
    ut_teardown();
}

/* T2, T5: a thread revoking its own address space, and destroying itself,
 * from inside its own syscall. */
static void test_syscall_lifecycle(void)
{
    uint64_t master = read_cr3() & 0x000FFFFFFFFFF000ull;

    /* Revoke the binding's parent from inside: the thread stops before
     * the syscall completes and never runs again. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(runner), false, NULL));
    struct cspace cs = task_cs(10);
    CHECK(cap_derive_to(&uts, 10 + T_AS, &cs, 1, CAP_R_ALL, 0) == T_OK);
    CHECK(thread_unbind(&uts, 10 + T_THR, TS_ASPACE) == T_OK);
    CHECK(thread_bind(&uts, 10 + T_THR, TS_ASPACE, &cs, 1) == T_OK);
    CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
    nreq = 0;
    REQ(10, SYS_CAP_REVOKE, 1, 0);
    REQ(10, SYS_TEST_REPORT, 0xBAD);
    run_user();
    CHECK(tstate(10) == THREAD_INACTIVE);
    CHECK(reqs(10)[1].result == REQ_SENTINEL && test_report_count == 0); /* never ran on */
    CHECK(cr3_is_kernel(master));
    ut_teardown();

    /* Delete the thread's only capability from inside its own syscall,
     * with another thread waiting: no use-after-free, the other runs. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(runner), false, NULL));
    CHECK(spawn(20, P(hello), false, NULL));
    cs = task_cs(10);
    CHECK(cap_derive_to(&uts, 10 + T_THR, &cs, 11, CAP_R_ALL, 0) == T_OK);
    struct thread *dying = thread_of(&ut_store[10 + T_THR]);
    CHECK(cap_delete(&uts, 10 + T_THR) == T_OK);   /* the task's copy is the last */
    nreq = 0;
    REQ(10, SYS_CAP_DELETE, 11);
    REQ(10, SYS_TEST_REPORT, 0xBAD);
    run_user();
    CHECK(cs.slots[11].type == CAP_NULL);
    CHECK(dying->regs.rax == 0xDDDDDDDDDDDDDDDDull);  /* no result written to the dead TCB */
    CHECK(test_report_count == 1 && test_reports[0].d == 0x7a72);  /* only hello reported */
    CHECK(this_cpu()->current == NULL && cr3_is_kernel(master));
    CHECK(cdt_consistent(&uts));
    ut_teardown();
}

/* T32: every entry path reaches common code in the same sanitised state:
 * AC = 0, DF = 0, the kernel's GS base. */
extern long copy_user_nmi_probe(void *dst, const void *src, size_t len, volatile uint32_t *icr);

#define AC_DF ((1u << 18) | (1u << 10))

static void test_entry_paths(void)
{
    CHECK(lapic_present());
    if (!lapic_present())
        return;

    /* NMI while the kernel runs (paranoid path, GS already the kernel's). */
    uint64_t nmis = entry_obs.nmis;
    lapic_self_nmi();
    CHECK(entry_obs.nmis == nmis + 1 && !entry_obs.nmi_last_from_user);
    CHECK(!(entry_obs.nmi_last_rflags & AC_DF) && entry_obs.nmi_gs_ok);
    CHECK(rdmsr(MSR_GS_BASE) == (uint64_t)this_cpu());

    /* An ordinary interrupt in kernel code, inside a one-instruction
     * interrupt window. */
    uint64_t kirqs = entry_obs.kernel_irqs;
    lapic_self_ipi(VEC_TEST_IPI);
    __asm__ volatile("sti; nop; cli" ::: "memory");
    CHECK(entry_obs.kernel_irqs == kirqs + 1);
    CHECK(!(entry_obs.kernel_irq_rflags & AC_DF) && entry_obs.kernel_irq_gs_ok);

    /* NMI inside the copy routine's open-SMAP window: the handler runs with
     * AC clear, and IRETQ restores AC so the interrupted copy completes (a
     * failure would fault on user memory at an unrecoverable RIP). */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(hello), false, NULL));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    uint8_t *data = phys_to_virt(cap_obj_paddr(&ut_store[10 + T_DATA]));
    for (int i = 0; i < 64; i++)
        data[i] = (uint8_t)(0xC0 + i);
    CHECK(vspace_activate(&uts, 10 + T_AS) == T_OK);
    memset(kbuf, 0, 64);
    nmis = entry_obs.nmis;
    long rc = copy_user_nmi_probe(kbuf, (const void *)(uintptr_t)UDATA_VA, 64, lapic_icr_lo_mmio());
    vspace_activate_kernel();
    CHECK(rc == 0 && memcmp(kbuf, data, 64) == 0);
    CHECK(entry_obs.nmis == nmis + 1);
    CHECK(!(entry_obs.nmi_last_rflags & AC_DF));          /* handler: AC clear */
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    CHECK(!(rflags & (1u << 18)));                        /* and closed after */
    ut_teardown();

    /* A timer interrupt while user code runs: an entry from ring 3 that is
     * neither a syscall nor a fault; the thread carries on to the end. */
    uint64_t uirqs = entry_obs.user_irqs, viol = entry_obs.violations;
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(spin), false, NULL));
    lapic_timer_oneshot(20000);
    run_user();
    lapic_timer_stop();
    CHECK(entry_obs.user_irqs >= uirqs + 1 && entry_obs.last_user_irq == VEC_TIMER);
    CHECK(test_report_count == 1 && test_reports[0].a == 20000000);
    CHECK(entry_obs.violations == viol);
    ut_teardown();
}

/* ---- IPC state machinery (docs/adr/0009-ipc.md, step 5.1) ---- */

static struct waitq *const tq_all[] = { &test_waitqs[0], &test_waitqs[1] };
#define AUDIT() (ipc_audit(tq_all, ARRAY_SIZE(tq_all)) && meta_audit())

/* Exact snapshot of all IPC-relevant state: every live thread's state,
 * links and saved registers, the run queue order, the test queues and the
 * current thread. Used to prove a failed operation changed nothing. */
#define SNAP_MAX 32
struct ipc_snap {
    unsigned n;
    struct {
        const struct thread *t;
        uint32_t state, reply_state, reply_flags;
        const void *wait_q, *wait_next, *wait_prev, *reply_to, *reply_from;
        struct user_regs regs;
        struct ipc_msg msg;
        struct ipc_offer offer;
        uint32_t ipc_flags;
    } th[SNAP_MAX];
    const struct thread *runq[SNAP_MAX];
    struct waitq q[TEST_WAITQS];
    const struct thread *current;
};
static struct ipc_snap snap_a, snap_b;

static void snapshot(struct ipc_snap *s, const struct thread *const *threads, unsigned n)
{
    memset(s, 0, sizeof(*s));
    s->n = n;
    for (unsigned i = 0; i < n && i < SNAP_MAX; i++) {
        const struct thread *t = threads[i];
        s->th[i].t = t;
        s->th[i].state = t->state;
        s->th[i].reply_state = t->reply_state;
        s->th[i].reply_flags = t->reply_flags;
        s->th[i].wait_q = t->wait_q;
        s->th[i].wait_next = t->wait_next;
        s->th[i].wait_prev = t->wait_prev;
        s->th[i].reply_to = t->reply_to;
        s->th[i].reply_from = t->reply_from;
        s->th[i].regs = t->regs;
        s->th[i].msg = t->msg;
        s->th[i].offer = t->offer;
        s->th[i].ipc_flags = t->ipc_flags;
    }
    unsigned k = 0;
    for (struct thread *t = runq_pop(); t; t = runq_pop()) {
        KASSERT(k < SNAP_MAX);                       /* bounded copy */
        s->runq[k++] = t;
    }
    for (unsigned i = 0; i < k; i++)                 /* restore the order */
        thread_enqueue((struct thread *)s->runq[i]);
    for (unsigned i = 0; i < TEST_WAITQS; i++)
        s->q[i] = test_waitqs[i];
    s->current = this_cpu()->current;
}

static struct thread *th(cap_slot_t b) { return thread_of(&ut_store[b + T_THR]); }

static void test_ipc_audit_detects(void)
{
    struct user_regs r0 = { .rdi = 0 };

    CHECK(ut_setup(20));
    CHECK(spawn(10, P(block), false, &r0));
    CHECK(spawn(20, P(block), false, &r0));
    run_user();
    CHECK(AUDIT());
    struct thread *a = th(10), *b = th(20);

    /* The audit must notice each kind of corruption. */
    a->state = THREAD_RUNNABLE;          /* runnable but not queued to run */
    CHECK(!AUDIT());
    a->state = THREAD_BLOCKED_RECV;
    CHECK(AUDIT());

    struct thread *tail = test_waitqs[0].tail;
    test_waitqs[0].tail = NULL;          /* broken queue */
    CHECK(!AUDIT());
    test_waitqs[0].tail = tail;

    static struct thread ghost;          /* not a live thread */
    ghost.state = THREAD_BLOCKED_RECV;
    ghost.wait_q = &test_waitqs[0];
    ghost.wait_prev = b;
    b->wait_next = &ghost;
    CHECK(!AUDIT());                     /* stale pointer in a queue */
    b->wait_next = NULL;

    a->reply_from = b;                   /* half a reply link */
    CHECK(!AUDIT());
    a->reply_from = NULL;
    CHECK(AUDIT());

    waitq_wake_all(&test_waitqs[0], T_EDEAD);
    run_user();
    ut_teardown();
    CHECK(AUDIT() && !test_waitqs[0].head);
}

static void test_ipc_states(void)
{
    struct user_regs r0 = { .rdi = 0 }, r1 = { .rdi = 1 }, rbad = { .rdi = 7 };

    CHECK(ut_setup(20));
    CHECK(spawn(10, P(block), false, &r0));
    CHECK(spawn(20, P(block), false, &r0));
    CHECK(spawn(30, P(block), false, &r0));
    run_user();

    /* Blocked means not runnable (P10); FIFO by arrival (P11). */
    struct thread *a = th(10), *b = th(20), *c = th(30);
    CHECK(runq_empty() && this_cpu()->current == NULL);
    CHECK(test_waitqs[0].head == a && a->wait_next == b && b->wait_next == c &&
          test_waitqs[0].tail == c);
    CHECK(tstate(10) == THREAD_BLOCKED_RECV && tstate(30) == THREAD_BLOCKED_RECV);
    CHECK(test_report_count == 0);
    CHECK(AUDIT());

    /* A failed blocking request changes nothing for anyone else. */
    const struct thread *others[] = { a, b, c };
    CHECK(spawn(40, P(block), false, &rbad));
    CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK);  /* exclude it from the run-queue view */
    snapshot(&snap_a, others, 3);
    CHECK(thread_resume(&uts, 40 + T_THR) == T_OK);
    run_user();
    snapshot(&snap_b, others, 3);
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_EINVAL);
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0);
    CHECK(AUDIT());

    /* Wake the head: result, state and run queue change together. */
    thread_wake(waitq_first(&test_waitqs[0]), 0x5150);
    CHECK(tstate(10) == THREAD_RUNNABLE && runq_contains(a) && a->regs.rax == 0x5150);
    CHECK(test_waitqs[0].head == b && !a->wait_q && !a->wait_next && !b->wait_prev);
    CHECK(AUDIT());
    run_user();
    CHECK(test_report_count == 1 && test_reports[0].a == 0x5150 && tstate(10) == THREAD_INACTIVE);

    /* Destroying a blocked thread in the middle relinks the queue. */
    CHECK(spawn(50, P(block), false, &r0));            /* queue: b c d */
    run_user();
    CHECK(cap_delete(&uts, 30 + T_THR) == T_OK);       /* c */
    CHECK(test_waitqs[0].head == b && b->wait_next == th(50) && th(50)->wait_prev == b);
    CHECK(AUDIT());

    /* Suspending a blocked thread: off the queue, T_EINTR on resume. */
    CHECK(thread_suspend(&uts, 20 + T_THR) == T_OK);
    CHECK(tstate(20) == THREAD_INACTIVE && !b->wait_q && test_waitqs[0].head == th(50));
    CHECK(AUDIT());
    CHECK(thread_resume(&uts, 20 + T_THR) == T_OK);
    run_user();
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_EINTR);

    /* Losing a required binding while blocked: the same, via T2. */
    CHECK(thread_unbind(&uts, 50 + T_THR, TS_ASPACE) == T_OK);
    CHECK(tstate(50) == THREAD_INACTIVE && !test_waitqs[0].head);
    CHECK(AUDIT());

    /* P6a: losing the IPC buffer aborts a receiver, not other waiters. */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 60, 2) == T_OK);
    CHECK(spawn(70, P(block), false, &r0));            /* BLOCKED_RECV */
    CHECK(spawn(80, P(block), false, &r1));            /* BLOCKED_NOTIF */
    CHECK(thread_bind(&uts, 70 + T_THR, TS_IPCBUF, &uts, 60) == T_OK);
    CHECK(thread_bind(&uts, 80 + T_THR, TS_IPCBUF, &uts, 61) == T_OK);
    run_user();
    CHECK(tstate(70) == THREAD_BLOCKED_RECV && tstate(80) == THREAD_BLOCKED_NOTIF);
    CHECK(thread_unbind(&uts, 70 + T_THR, TS_IPCBUF) == T_OK);
    CHECK(thread_unbind(&uts, 80 + T_THR, TS_IPCBUF) == T_OK);
    CHECK(tstate(70) == THREAD_INACTIVE && th(70)->regs.rax == (uint64_t)(int64_t)T_EINTR);
    CHECK(tstate(80) == THREAD_BLOCKED_NOTIF);
    CHECK(AUDIT());

    /* Object death wakes *every* waiter with T_EDEAD (P26): three of them,
     * so waking only the first cannot pass. */
    CHECK(spawn(90, P(block), false, &r1));
    CHECK(spawn(100, P(block), false, &r1));
    run_user();
    CHECK(tstate(80) == THREAD_BLOCKED_NOTIF && tstate(90) == THREAD_BLOCKED_NOTIF &&
          tstate(100) == THREAD_BLOCKED_NOTIF);
    waitq_wake_all(&test_waitqs[1], T_EDEAD);
    CHECK(!test_waitqs[1].head && !test_waitqs[1].tail);
    CHECK(tstate(80) == THREAD_RUNNABLE && tstate(90) == THREAD_RUNNABLE &&
          tstate(100) == THREAD_RUNNABLE);
    CHECK(AUDIT());
    run_user();
    CHECK(test_report_count == 3);
    for (unsigned i = 0; i < test_report_count; i++)
        CHECK((int64_t)test_reports[i].a == T_EDEAD);
    CHECK(AUDIT());

    ut_teardown();
    CHECK(AUDIT() && runq_empty());
}

/* P16, P17, P28, P29: reply links when either side disappears. */
static void test_ipc_reply_links(void)
{
    CHECK(ut_setup(20));
    /* Servers 10, 40 and 70 are parked (blocked), never stopped: a stopped
     * thread holds no reply authority (P64, audit rule 11). Callers start
     * stopped; reply_link() makes them BLOCKED_REPLY. */
    for (cap_slot_t b = 10; b <= 70; b += 10) {
        if (b == 10 || b == 40 || b == 70) {
            CHECK(spawn(b, P(block), false, &(struct user_regs){ .rdi = 1 }));
        } else {
            CHECK(spawn(b, P(hello), false, NULL));
            CHECK(thread_suspend(&uts, b + T_THR) == T_OK);
        }
    }
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_NOTIF && tstate(70) == THREAD_BLOCKED_NOTIF);

    /* Caller destroyed: the server keeps its authority, marked gone. */
    reply_link(th(10), th(20), REPLY_CALL_GRANT);
    CHECK(tstate(20) == THREAD_BLOCKED_REPLY && th(10)->reply_to == th(20));
    CHECK(AUDIT());
    CHECK(cap_delete(&uts, 20 + T_THR) == T_OK);
    CHECK(th(10)->reply_state == REPLY_CALLER_GONE && th(10)->reply_to == NULL);
    CHECK(AUDIT());
    th(10)->reply_state = REPLY_NONE;    /* (a reply would consume it, step 5.5) */

    /* Server suspended: the caller wakes with T_EDEAD. */
    reply_link(th(10), th(30), 0);
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(tstate(30) == THREAD_RUNNABLE && runq_contains(th(30)));
    CHECK(th(30)->regs.rax == (uint64_t)(int64_t)T_EDEAD && th(30)->reply_from == NULL);
    CHECK(th(10)->reply_state == REPLY_NONE && th(10)->reply_to == NULL);
    CHECK(AUDIT());
    CHECK(thread_suspend(&uts, 30 + T_THR) == T_OK);

    /* Server destroyed: the same. */
    reply_link(th(40), th(50), 0);
    CHECK(cap_delete(&uts, 40 + T_THR) == T_OK);
    CHECK(tstate(50) == THREAD_RUNNABLE && th(50)->regs.rax == (uint64_t)(int64_t)T_EDEAD);
    CHECK(AUDIT());
    CHECK(thread_suspend(&uts, 50 + T_THR) == T_OK);

    /* Caller suspended: T_EINTR for it, "caller gone" for the server. */
    reply_link(th(70), th(60), 0);
    CHECK(thread_suspend(&uts, 60 + T_THR) == T_OK);
    CHECK(tstate(60) == THREAD_INACTIVE && th(60)->regs.rax == (uint64_t)(int64_t)T_EINTR);
    CHECK(th(70)->reply_state == REPLY_CALLER_GONE && th(60)->reply_from == NULL);
    CHECK(AUDIT());
    /* P64: stopping the server is terminal for the gone authority too. */
    CHECK(thread_suspend(&uts, 70 + T_THR) == T_OK);
    CHECK(th(70)->reply_state == REPLY_NONE && AUDIT());

    ut_teardown();
    CHECK(AUDIT() && runq_empty());
}

/* ---- Notifications (ADR 0009, P1, P20-P22, P27, P27a; step 5.2) ---- */

/* Independent count of signal authority: scan every capability table the
 * test uses, rather than trusting the kernel's walk. */
static unsigned scan_write_caps(const struct cap *obj, struct cspace *const *css, unsigned ncs)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ncs; i++)
        for (uint32_t j = 0; j < css[i]->nslots; j++) {
            const struct cap *c = &css[i]->slots[j];
            if (c->type == CAP_NOTIFICATION && c->object == obj->object && (c->rights & CAP_R_WRITE))
                n++;
        }
    return n;
}

#define W_ (CAP_R_WRITE | CAP_R_GRANT)

/* Structural invariant: a signal that finds a waiter consumes every
 * pending bit, so waiters and pending bits never coexist. */
static bool notif_ok(const struct notification *n)
{
    struct waitq *q[] = { (struct waitq *)&n->waiters };
    return !(n->waiters.head && n->pending) && ipc_audit(q, 1) && meta_audit();
}

/* Master notification in uts slot 1 (unbadged, all rights): minting
 * authority. Badged signal capabilities derive from it. */
static struct notification *ntfn_setup(void)
{
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &uts, 1, 1) == T_OK);
    return notification_of(&ut_store[1]);
}

static void test_notif_semantics(void)
{
    CHECK(ut_setup(20));
    struct notification *n = ntfn_setup();
    CHECK(n->waiters.kind == THREAD_BLOCKED_NOTIF && n->pending == 0);

    CHECK(spawn(10, P(runner), false, NULL));
    struct cspace cs = task_cs(10);
    CHECK(cap_derive_to(&uts, 1, &cs, 1, W_, 0x01) == T_OK);              /* A */
    CHECK(cap_derive_to(&uts, 1, &cs, 2, W_, 0x04) == T_OK);              /* B */
    CHECK(cap_derive_to(&uts, 1, &cs, 3, W_, 0x80) == T_OK);              /* C */
    CHECK(cap_derive_to(&uts, 1, &cs, 4, CAP_R_READ, 0) == T_OK);         /* waiter side */
    CHECK(cap_derive_to(&uts, 1, &cs, 5, CAP_R_WRITE, 0) == T_OK);        /* unbadged W */
    CHECK(cap_derive_to(&uts, 1, &cs, 6, CAP_R_READ, 0x02) == T_OK);      /* badged, no W */

    nreq = 0;
    REQ(10, SYS_SIGNAL, 1, 0x04);       /* 0: A, with a forged "bits" argument */
    REQ(10, SYS_POLL, 4);               /* 1: only 0x01 */
    REQ(10, SYS_SIGNAL, 1);             /* 2 */
    REQ(10, SYS_SIGNAL, 1);             /* 3 */
    REQ(10, SYS_SIGNAL, 2);             /* 4 */
    REQ(10, SYS_POLL, 4);               /* 5: 0x05, not three events */
    REQ(10, SYS_POLL, 4);               /* 6: nothing hidden underneath */
    REQ(10, SYS_SIGNAL, 5);             /* 7: unbadged: refused */
    REQ(10, SYS_SIGNAL, 6);             /* 8: badged but no WRITE: refused */
    REQ(10, SYS_WAIT, 1);               /* 9: no READ: refused */
    REQ(10, SYS_SIGNAL, 3);             /* 10 */
    REQ(10, SYS_POLL, 4);               /* 11: only 0x80: 7-9 added nothing */
    REQ(10, SYS_SIGNAL, 9);             /* 12: empty slot */
    REQ(10, SYS_WAIT, 4);               /* 13: bits pending? none: would block... */
    reqs(10)[13].nr = SYS_POLL;         /* ...so poll instead: 0 */
    run_user();

    static const struct { int64_t rax; uint64_t rsi; bool check_rsi; } want[] = {
        { T_OK, 0, false }, { T_OK, 0x01, true }, { T_OK, 0, false }, { T_OK, 0, false },
        { T_OK, 0, false }, { T_OK, 0x05, true }, { T_OK, 0, true }, { T_EPERM, 0, false },
        { T_EPERM, 0, false }, { T_EPERM, 0, false }, { T_OK, 0, false }, { T_OK, 0x80, true },
        { T_EBADCAP, 0, false }, { T_OK, 0, true },
    };
    CHECK(nreq == ARRAY_SIZE(want));
    for (unsigned i = 0; i < ARRAY_SIZE(want); i++) {
        CHECK(res(10, i) == want[i].rax);
        if (want[i].check_rsi)
            CHECK(reqs(10)[i].a[5] == want[i].rsi);
        if (res(10, i) != want[i].rax || (want[i].check_rsi && reqs(10)[i].a[5] != want[i].rsi))
            kprintf("    request %u: rax %ld rsi %#lx\n", i, res(10, i), reqs(10)[i].a[5]);
    }
    CHECK(n->pending == 0 && AUDIT());

    /* Failed signals change nothing at all: exact snapshot, kernel API. */
    n->pending = 0x22;
    struct notification before = *n;
    CHECK(notification_signal(NULL, &cs, 5) == T_EPERM);     /* unbadged */
    CHECK(notification_signal(NULL, &cs, 4) == T_EPERM);     /* READ only, unbadged */
    CHECK(notification_signal(NULL, &cs, 6) == T_EPERM);     /* READ only, badged */
    CHECK(notification_signal(NULL, &cs, 77) == T_ERANGE);
    CHECK(memcmp(&before, n, sizeof(before)) == 0);
    n->pending = 0;

    ut_teardown();
    CHECK(AUDIT());
}

/* P1: badge semantics across derivation are explicit, not accidental. */
static void test_notif_badges(void)
{
    struct cap_info info;

    CHECK(ut_setup(20));
    struct notification *n = ntfn_setup();
    CHECK(cap_derive(&uts, 1, 2, W_, 0x01) == T_OK);       /* A, minted by the master */

    CHECK(cap_derive(&uts, 2, 3, W_, 0) == T_OK);          /* copy of A */
    CHECK(cap_identify(&uts, 3, &info) == T_OK && info.badge == 0x01);   /* inherited */
    CHECK(cap_derive(&uts, 2, 4, W_, 0x04) == T_EPERM);    /* A cannot re-badge */
    CHECK(cap_derive(&uts, 3, 4, W_, 0xFF) == T_EPERM);    /* nor can its copies */
    CHECK(ut_store[4].type == CAP_NULL);
    CHECK(cap_derive(&uts, 1, 4, W_, 0x10) == T_OK);       /* only the master mints */
    CHECK(cap_derive(&uts, 1, 5, W_, 0) == T_OK);          /* unbadged copy of the master */
    CHECK(cap_identify(&uts, 5, &info) == T_OK && info.badge == 0);
    CHECK(cap_derive(&uts, 5, 6, W_, 0x20) == T_OK);       /* which can mint too */

    CHECK(notification_signal(NULL, &uts, 3) == T_OK);     /* the copy signals A's bit */
    CHECK(n->pending == 0x01);
    CHECK(notification_signal(NULL, &uts, 4) == T_OK && n->pending == 0x11);
    CHECK(notification_signal(NULL, &uts, 6) == T_OK && n->pending == 0x31);
    CHECK(notification_signal(NULL, &uts, 5) == T_EPERM && n->pending == 0x31);
    CHECK(notification_signal(NULL, &uts, 1) == T_EPERM && n->pending == 0x31);
    ut_teardown();
}

/* Wait before signal: FIFO waiters, each signal consumed by exactly one
 * waiter, and bits with no waiter stay pending. */
static void test_notif_waiters(void)
{
    struct user_regs w = { .r12 = SYS_WAIT, .rdi = 1 };

    CHECK(ut_setup(20));
    struct notification *n = ntfn_setup();
    CHECK(spawn(30, P(sys1), false, &w));
    CHECK(spawn(40, P(sys1), false, &w));
    CHECK(spawn(10, P(runner), false, NULL));
    struct cspace cs = task_cs(10), c30 = task_cs(30), c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 1, &c30, 1, CAP_R_READ, 0) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &c40, 1, CAP_R_READ, 0) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &cs, 1, W_, 0x01) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &cs, 2, W_, 0x04) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &cs, 3, W_, 0x80) == T_OK);
    nreq = 0;
    REQ(10, SYS_SIGNAL, 1);     /* wakes the first waiter with 0x01 */
    REQ(10, SYS_SIGNAL, 3);     /* wakes the second with 0x80 */
    REQ(10, SYS_SIGNAL, 2);     /* nobody waits: stays pending */

    /* Run only the waiters first: both block, in order. */
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    run_user();
    CHECK(tstate(30) == THREAD_BLOCKED_NOTIF && tstate(40) == THREAD_BLOCKED_NOTIF);
    CHECK(n->waiters.head == th(30) && n->waiters.tail == th(40));
    CHECK(notif_ok(n));

    CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
    run_user();
    CHECK(test_report_count == 2);
    CHECK(test_reports[0].a == T_OK && test_reports[0].b == 0x01);
    CHECK(test_reports[1].a == T_OK && test_reports[1].b == 0x80);
    CHECK(n->pending == 0x04 && !n->waiters.head);
    CHECK(notif_ok(n));
    ut_teardown();
}

/* P27, P27a: waiters are woken when signal authority is gone, and only
 * then; the count is of live WRITE records, whatever the CDT shape. */
static void test_notif_liveness(void)
{
    struct user_regs w = { .r12 = SYS_WAIT, .rdi = 1 };

    CHECK(ut_setup(20));
    struct notification *n = ntfn_setup();
    CHECK(spawn(30, P(sys1), false, &w));
    struct cspace c30 = task_cs(30);
    struct cspace *all[] = { &uts, &c30 };
    CHECK(cap_derive_to(&uts, 1, &c30, 1, CAP_R_READ | CAP_R_GRANT, 0) == T_OK);
    CHECK(cap_derive(&uts, 1, 2, W_, 0x01) == T_OK);          /* A, child of master */
    CHECK(cap_derive(&uts, 2, 3, W_, 0) == T_OK);             /* A', child of A */
    CHECK(cap_derive(&uts, 1, 4, W_, 0x04) == T_OK);          /* B */
    CHECK(cap_derive_to(&c30, 1, &c30, 2, CAP_R_READ, 0) == T_OK);  /* more READ */
    run_user();
    CHECK(tstate(30) == THREAD_BLOCKED_NOTIF);
    struct waitq *nq[] = { &n->waiters };

#define COUNTS(k) (notification_signal_caps(&c30.slots[1]) == (k) && scan_write_caps(&c30.slots[1], all, 2) == (k))
    CHECK(COUNTS(4));
    /* Deleting the master re-parents its children: the count is still the
     * number of live WRITE records, and nobody is woken. */
    CHECK(cap_delete(&uts, 1) == T_OK);
    CHECK(COUNTS(3) && tstate(30) == THREAD_BLOCKED_NOTIF && notif_ok(n));
    CHECK(cap_delete(&uts, 2) == T_OK);                       /* A: A' re-parents */
    CHECK(COUNTS(2) && tstate(30) == THREAD_BLOCKED_NOTIF);
    CHECK(cap_delete(&c30, 2) == T_OK);                       /* a READ cap: irrelevant */
    CHECK(COUNTS(2) && tstate(30) == THREAD_BLOCKED_NOTIF);
    CHECK(cap_delete(&uts, 4) == T_OK);
    CHECK(COUNTS(1) && tstate(30) == THREAD_BLOCKED_NOTIF);
    /* The last signal authority goes: the waiter wakes with T_EDEAD. */
    CHECK(cap_delete(&uts, 3) == T_OK);
    /* SC23: authority loss lists the live object with its doomed
     * direction; the reaper fails the waiter. */
    CHECK(COUNTS(0) && tstate(30) == THREAD_BLOCKED_NOTIF && n->waiters.head == th(30));
    CHECK((n->meta.flags & (OBJ_LISTED | OBJ_DOOM_RECV)) == (OBJ_LISTED | OBJ_DOOM_RECV));
    reap_all();
    CHECK(COUNTS(0) && tstate(30) == THREAD_RUNNABLE && !n->waiters.head);
    CHECK(!(n->meta.flags & (OBJ_LISTED | OBJ_DOOM_RECV | OBJ_DYING)));
    CHECK(ipc_audit(nq, 1));
    run_user();
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_EDEAD);

    /* A later wait fails at once; poll still works and returns nothing. */
    struct cspace cs;
    CHECK(spawn(40, P(runner), false, NULL));
    cs = task_cs(40);
    CHECK(cap_derive_to(&c30, 1, &cs, 1, CAP_R_READ, 0) == T_OK);
    nreq = 0;
    REQ(40, SYS_WAIT, 1);
    REQ(40, SYS_POLL, 1);
    run_user();
    CHECK(res(40, 0) == T_EDEAD && res(40, 1) == T_OK && reqs(40)[1].a[5] == 0);
#undef COUNTS
    ut_teardown();
}

/* Destroying a waiting thread, and the notification itself. */
static void test_notif_destroy(void)
{
    struct user_regs w = { .r12 = SYS_WAIT, .rdi = 1 };

    CHECK(ut_setup(20));
    struct notification *n = ntfn_setup();
    CHECK(spawn(30, P(sys1), false, &w));
    CHECK(spawn(40, P(sys1), false, &w));
    CHECK(spawn(50, P(sys1), false, &w));
    for (cap_slot_t b = 30; b <= 50; b += 10) {
        struct cspace c = task_cs(b);
        CHECK(cap_derive_to(&uts, 1, &c, 1, CAP_R_READ, 0) == T_OK);
    }
    run_user();
    struct waitq *nq[] = { &n->waiters };
    CHECK(n->waiters.head == th(30) && n->waiters.tail == th(50));
    CHECK(cap_delete(&uts, 40 + T_THR) == T_OK);              /* the middle waiter */
    CHECK(n->waiters.head == th(30) && th(30)->wait_next == th(50) && ipc_audit(nq, 1));
    /* Destroy the object with waiters: every capability goes. */
    for (cap_slot_t b = 30; b <= 50; b += 20) {
        struct cspace c = task_cs(b);
        CHECK(cap_delete(&c, 1) == T_OK);
    }
    CHECK(tstate(30) == THREAD_BLOCKED_NOTIF);   /* the master (WRITE) still exists */
    CHECK(cap_delete(&uts, 1) == T_OK);          /* last WRITE and last cap */
    /* SC23: dying, with its waiters still queued; the reaper fails them
     * one per unit, and only then is the memory released. */
    CHECK(tstate(30) == THREAD_BLOCKED_NOTIF && tstate(50) == THREAD_BLOCKED_NOTIF);
    CHECK((n->meta.flags & OBJ_LISTED) && notif_ok(n));
    reap_all();
    CHECK(tstate(30) == THREAD_RUNNABLE && tstate(50) == THREAD_RUNNABLE);
    CHECK(!(n->meta.flags & OBJ_LISTED) && ut_store[0].ut.dying == 0);
    run_user();
    CHECK(test_report_count == 2 && (int64_t)test_reports[0].a == T_EDEAD &&
          (int64_t)test_reports[1].a == T_EDEAD);
    ut_teardown();
    CHECK(AUDIT());
}

/* ---- Endpoint rendezvous (ADR 0009, step 5.3) ---- */

#define RW_ (CAP_R_READ | CAP_R_WRITE)
#define MSG_INFO(label, len) (((uint64_t)(label) << 32) | (len))

/* Endpoint invariants: both queues well-formed and audited, and never both
 * non-empty (a receiver arriving while senders wait rendezvouses). */
static bool ep_ok(const struct endpoint *e)
{
    struct waitq *q[] = { (struct waitq *)&e->senders, (struct waitq *)&e->receivers };
    return !(e->senders.head && e->receivers.head) && ipc_audit(q, 2) && meta_audit();
}

static struct ipc_buffer *tbuf(cap_slot_t b)
{
    return phys_to_virt(cap_obj_paddr(&ut_store[b + T_DATA]));
}

/* A thread at uts slots b.. that performs one SEND (with `tag` as its only
 * word) or one RECV on its cspace slot 1, with its data page bound as its
 * IPC buffer, and the endpoint capability given with `rights`. */
static bool ep_thread_on(cap_slot_t b, cap_slot_t ep, bool send, uint64_t label, uint64_t tag,
                         cap_rights_t rights)
{
    struct user_regs r = { .r12 = send ? SYS_SEND : SYS_RECV, .rdi = 1,
                           .rsi = MSG_INFO(label, 1) };
    if (!spawn(b, P(sys1), false, &r))
        return false;
    struct cspace cs = task_cs(b);
    tbuf(b)->words[0] = send ? tag : 0xAAAAAAAAAAAAAAAAull;
    return cap_derive_to(&uts, ep, &cs, 1, rights, 0) == T_OK &&
           thread_bind(&uts, b + T_THR, TS_IPCBUF, &uts, b + T_DATA) == T_OK;
}

/* A thread at b using the endpoint in uts slot 1, from its cspace slot 1. */
static bool ep_thread(cap_slot_t b, bool send, uint64_t label, uint64_t tag, cap_rights_t rights)
{
    return ep_thread_on(b, 1, send, label, tag, rights);
}

static uint64_t got(cap_slot_t b) { return tbuf(b)->words[0]; }

/* Run threads one at a time in the given order, so each blocks (or
 * rendezvouses) before the next starts; then let everything finish. */
static void run_in_order(const cap_slot_t *order, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        CHECK(thread_suspend(&uts, order[i] + T_THR) == T_OK);
    for (unsigned i = 0; i < n; i++) {
        CHECK(thread_resume(&uts, order[i] + T_THR) == T_OK);
        sched_run();
    }
}

/* P39, P41, P42: FIFO across three orders. S_i carries tag 0x50+i; R_i must
 * receive exactly S_i's tag and label. */
static void ep_fifo_case(const cap_slot_t *order)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    const cap_slot_t S[3] = { 10, 20, 30 }, R[3] = { 40, 50, 60 };
    for (unsigned i = 0; i < 3; i++) {
        CHECK(ep_thread(S[i], true, i + 1, 0x51 + i, CAP_R_WRITE));
        CHECK(ep_thread(R[i], false, 0, 0, CAP_R_READ));
    }
    test_report_count = 0;
    run_in_order(order, 6);
    CHECK(ep_ok(e) && !e->senders.head && !e->receivers.head);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(got(R[i]) == 0x51 + i);                       /* S_i <-> R_i */
        CHECK(tstate(S[i]) == THREAD_INACTIVE && tstate(R[i]) == THREAD_INACTIVE);
        CHECK(th(S[i])->msg.info == 0);                     /* nothing left (P42) */
    }
    /* Every one of the six operations completed with status 0. */
    CHECK(test_report_count == 6);
    for (unsigned i = 0; i < test_report_count; i++)
        CHECK(test_reports[i].a == T_OK);
    ut_teardown();
}

static void test_ep_fifo(void)
{
    static const cap_slot_t senders_first[6] = { 10, 20, 30, 40, 50, 60 };
    static const cap_slot_t receivers_first[6] = { 40, 50, 60, 10, 20, 30 };
    static const cap_slot_t interleaved[6] = { 10, 20, 40, 30, 50, 60 };
    ep_fifo_case(senders_first);
    ep_fifo_case(receivers_first);
    ep_fifo_case(interleaved);
}

/* P3, P4, P7, P47: message shape; failures with partners queued leave an
 * exact snapshot unchanged (validation happens before any dequeue). */
static void test_ep_format(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);

    /* A receiver waits; a sender sends 32 words. */
    CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
    for (unsigned i = 0; i < MSG_MAX_WORDS; i++)
        tbuf(40)->words[i] = 0xAA;
    tbuf(40)->caps[0] = 0xCC;
    run_user();
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && ep_ok(e));
    CHECK(spawn(10, P(runner), false, NULL));
    struct cspace cs = task_cs(10);
    CHECK(cap_derive_to(&uts, 1, &cs, 1, RW_, 0) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &cs, 2, CAP_R_READ, 0) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 70, 1) == T_OK);
    struct ipc_buffer *sb = phys_to_virt(cap_obj_paddr(&ut_store[70]));
    CHECK(thread_bind(&uts, 10 + T_THR, TS_IPCBUF, &uts, 70) == T_OK);
    for (unsigned i = 0; i < MSG_MAX_WORDS; i++)
        sb->words[i] = 0x1000 + i;

    /* Kernel-API failures while the receiver waits: nothing changes. */
    const struct thread *who[] = { th(40) };
    struct waitq *eq[] = { &e->senders, &e->receivers };
    struct endpoint e0 = *e;
    snapshot(&snap_a, who, 1);
    struct thread *snd = th(10);
    static const uint64_t bad_info[] = {
        MSG_INFO(1, 33), MSG_INFO(1, 1) | (1u << 8), MSG_INFO(1, 1) | (1ull << 31),
    };
    for (unsigned i = 0; i < ARRAY_SIZE(bad_info); i++)
        CHECK(endpoint_send(snd, &cs, 1, bad_info[i]) == T_EINVAL);
    CHECK(endpoint_send(snd, &cs, 1, MSG_INFO(1, 1) | (1u << 6)) == T_EPERM);  /* caps, no GRANT */
    CHECK(endpoint_send(snd, &cs, 2, MSG_INFO(1, 1)) == T_EPERM);      /* READ only */
    CHECK(endpoint_send(snd, &cs, 9, MSG_INFO(1, 1)) == T_EBADCAP);
    snapshot(&snap_b, who, 1);
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);
    CHECK(tbuf(40)->words[0] == 0xAA && ipc_audit(eq, 2));

    /* The real send, from ring 3: all 32 words arrive. */
    nreq = 0;
    REQ(10, SYS_SEND, 1, MSG_INFO(7, 32));
    run_user();
    CHECK(res(10, 0) == T_OK && test_report_count == 1);
    CHECK(test_reports[0].a == T_OK && test_reports[0].b == MSG_INFO(7, 32));
    bool all = true;
    for (unsigned i = 0; i < MSG_MAX_WORDS; i++)
        all &= tbuf(40)->words[i] == 0x1000 + i;
    CHECK(all && tbuf(40)->caps[0] == 0xCC);       /* nothing beyond the words */
    CHECK(ep_ok(e));

    ut_teardown();

    /* Length 1 into a dirty buffer: only word 0 is written. */

    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
    for (unsigned i = 0; i < MSG_MAX_WORDS; i++)
        tbuf(40)->words[i] = 0xAA;
    CHECK(ep_thread(10, true, 3, 0x77, CAP_R_WRITE));
    run_in_order((const cap_slot_t[]){ 40, 10 }, 2);
    CHECK(tbuf(40)->words[0] == 0x77 && tbuf(40)->words[1] == 0xAA &&
          tbuf(40)->words[31] == 0xAA);
    ut_teardown();

    /* Length 0 with no IPC buffer is fine for a sender; a receiver always
     * needs one; data without a buffer is refused. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(spawn(10, P(runner), false, NULL));
    cs = task_cs(10);
    CHECK(cap_derive_to(&uts, 1, &cs, 1, RW_, 0) == T_OK);
    nreq = 0;
    REQ(10, SYS_RECV, 1);                  /* 0: no buffer */
    REQ(10, SYS_SEND, 1, MSG_INFO(1, 1));  /* 1: data, no buffer */
    run_user();
    CHECK(res(10, 0) == T_EINVAL && res(10, 1) == T_EINVAL);
    CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
    tbuf(40)->words[0] = 0xAA;
    run_user();                            /* the receiver blocks */
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && ep_ok(e));
    CHECK(spawn(20, P(runner), false, NULL));
    struct cspace c20 = task_cs(20);
    CHECK(cap_derive_to(&uts, 1, &c20, 1, CAP_R_WRITE, 0) == T_OK);
    nreq = 0;
    REQ(20, SYS_SEND, 1, MSG_INFO(9, 0));  /* label only, no IPC buffer */
    run_user();
    CHECK(res(20, 0) == T_OK);
    CHECK(test_report_count == 1 && test_reports[0].b == MSG_INFO(9, 0));
    CHECK(tbuf(40)->words[0] == 0xAA);     /* zero words: buffer untouched */
    CHECK(ep_ok(e));
    ut_teardown();
}

/* P46, P6a: the blocked sender's message lives in the kernel. */
static void test_ep_capture(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(ep_thread(10, true, 1, 0x1111, CAP_R_WRITE));
    CHECK(ep_thread(20, true, 2, 0x2222, CAP_R_WRITE));
    CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
    CHECK(ep_thread(50, false, 0, 0, CAP_R_READ));
    CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK && thread_suspend(&uts, 50 + T_THR) == T_OK);
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && tstate(20) == THREAD_BLOCKED_SEND && ep_ok(e));
    CHECK(th(10)->msg.info == MSG_INFO(1, 1) && th(10)->msg.words[0] == 0x1111);
    /* P9: resuming a blocked thread is refused: it is already started,
     * and making it runnable would leave it in its queue. */
    CHECK(thread_resume(&uts, 10 + T_THR) == T_EBUSY);
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && !runq_contains(th(10)) && ep_ok(e));

    /* P47: refused receives, with senders queued, change nothing: no READ
     * (only WRITE); holding reply authority, pending or caller-gone. */
    struct cspace c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 1, &c40, 2, CAP_R_WRITE, 0) == T_OK);
    reply_link(th(40), th(50), 0);
    const struct thread *who[] = { th(10), th(20), th(40), th(50) };
    struct endpoint e0 = *e;
    snapshot(&snap_a, who, 4);
    CHECK(endpoint_recv(th(40), &c40, 2) == T_EPERM);
    CHECK(endpoint_recv(th(40), &c40, 1) == T_EBUSY);
    snapshot(&snap_b, who, 4);
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);
    CHECK(thread_suspend(&uts, 50 + T_THR) == T_OK);             /* caller gone */
    CHECK(th(40)->reply_state == REPLY_CALLER_GONE);
    CHECK(endpoint_recv(th(40), &c40, 1) == T_EBUSY && e->senders.head == th(10));
    /* (Built by hand on a stopped thread, a state P64 rules out: undo it
     * before the audit. reply_gone tests this with a real server.) */
    th(40)->reply_state = REPLY_NONE;
    CHECK(ep_ok(e));

    tbuf(10)->words[0] = 0xBADBAD;                               /* rewritten while blocked */
    CHECK(thread_unbind(&uts, 20 + T_THR, TS_IPCBUF) == T_OK);   /* unbound while blocked */
    CHECK(tstate(20) == THREAD_BLOCKED_SEND);                    /* not aborted (P6a) */
    CHECK(thread_resume(&uts, 40 + T_THR) == T_OK && thread_resume(&uts, 50 + T_THR) == T_OK);
    run_user();
    CHECK(got(40) == 0x1111 && got(50) == 0x2222);
    CHECK(ep_ok(e) && !e->senders.head);
    ut_teardown();
}

/* P43, P44, P45: liveness in both directions, counted semantically. */
static unsigned scan_ep_caps(const struct cap *obj, cap_rights_t right,
                             struct cspace *const *css, unsigned ncs)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ncs; i++)
        for (uint32_t j = 0; j < css[i]->nslots; j++) {
            const struct cap *c = &css[i]->slots[j];
            if (c->type == CAP_ENDPOINT && c->object == obj->object && (c->rights & right))
                n++;
        }
    return n;
}

static void test_ep_liveness(void)
{
    /* A blocked sender waits for a receiver: deleting every send
     * capability, its own included, must not wake it; it still delivers. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(ep_thread(10, true, 1, 0x5151, CAP_R_WRITE));
    CHECK(cap_derive(&uts, 1, 2, CAP_R_READ | CAP_R_GRANT, 0) == T_OK);   /* receive authority */
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND);
    struct cspace c10 = task_cs(10);
    CHECK(cap_delete(&c10, 1) == T_OK);              /* its own send capability */
    CHECK(cap_delete(&uts, 1) == T_OK);              /* the master (re-parents) */
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && ep_ok(e));
    CHECK(spawn(40, P(sys1), false, &(struct user_regs){ .r12 = SYS_RECV, .rdi = 1 }));
    struct cspace c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 2, &c40, 1, CAP_R_READ, 0) == T_OK);
    CHECK(thread_bind(&uts, 40 + T_THR, TS_IPCBUF, &uts, 40 + T_DATA) == T_OK);
    run_user();
    CHECK(got(40) == 0x5151 && ep_ok(e));
    ut_teardown();

    /* Last receive authority gone: blocked senders get T_EDEAD; deleting
     * a re-parented master or extra send capabilities wakes nobody. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(cap_derive(&uts, 1, 2, CAP_R_READ | CAP_R_GRANT, 0) == T_OK);
    CHECK(cap_derive(&uts, 2, 3, CAP_R_READ, 0) == T_OK);
    CHECK(ep_thread(10, true, 1, 1, CAP_R_WRITE));
    CHECK(ep_thread(20, true, 1, 2, CAP_R_WRITE));
    run_user();
    struct cspace c20 = task_cs(20);
    c10 = task_cs(10);
    struct cspace *all[] = { &uts, &c10, &c20 };
    /* Count through a capability that stays alive throughout (sender 10's
     * own), never through one the test deletes. */
#define EPC(r, k) (endpoint_caps_with(&c10.slots[1], r) == (k) && scan_ep_caps(&c10.slots[1], r, all, 3) == (k))
    CHECK(EPC(CAP_R_READ, 3) && EPC(CAP_R_WRITE, 3));
    CHECK(cap_delete(&uts, 1) == T_OK);              /* master: READ+WRITE */
    CHECK(EPC(CAP_R_READ, 2) && tstate(10) == THREAD_BLOCKED_SEND && ep_ok(e));
    CHECK(cap_delete(&c20, 1) == T_OK);              /* a send cap: irrelevant to senders */
    CHECK(EPC(CAP_R_WRITE, 1) && tstate(10) == THREAD_BLOCKED_SEND);
    CHECK(cap_delete(&uts, 2) == T_OK);              /* 3 re-parents */
    CHECK(EPC(CAP_R_READ, 1) && tstate(10) == THREAD_BLOCKED_SEND);
    CHECK(cap_delete(&uts, 3) == T_OK);              /* the last READ */
    /* Doomed, not woken in this entry (SC23): the reaper fails the
     * senders one per unit, each exactly once. */
    CHECK(EPC(CAP_R_READ, 0) && e->senders.head == th(10) && tstate(10) == THREAD_BLOCKED_SEND);
    CHECK((e->meta.flags & (OBJ_LISTED | OBJ_DOOM_SEND)) == (OBJ_LISTED | OBJ_DOOM_SEND));
    CHECK(reaper_run(1) == 1 && tstate(10) == THREAD_RUNNABLE && tstate(20) == THREAD_BLOCKED_SEND);
    reap_all();
    CHECK(EPC(CAP_R_READ, 0) && !e->senders.head && !(e->meta.flags & OBJ_LISTED));
    CHECK(tstate(10) == THREAD_RUNNABLE && tstate(20) == THREAD_RUNNABLE && ep_ok(e));
    run_user();
    CHECK(test_report_count == 2 && (int64_t)test_reports[0].a == T_EDEAD &&
          (int64_t)test_reports[1].a == T_EDEAD);
    /* A later send fails at once: nobody can ever receive. */
    CHECK(endpoint_send(th(10), &c10, 1, MSG_INFO(1, 0)) == T_EDEAD);
#undef EPC
    ut_teardown();

    /* Last send authority gone: blocked receivers get T_EDEAD. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(cap_derive(&uts, 1, 2, CAP_R_WRITE, 0) == T_OK);
    CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
    CHECK(ep_thread(50, false, 0, 0, CAP_R_READ));
    run_user();
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && tstate(50) == THREAD_BLOCKED_RECV);
    struct cspace c50 = task_cs(50);
    CHECK(cap_delete(&c50, 1) == T_OK);              /* a READ cap: irrelevant */
    CHECK(cap_delete(&uts, 1) == T_OK);              /* master */
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && tstate(50) == THREAD_BLOCKED_RECV && ep_ok(e));
    CHECK(cap_delete(&uts, 2) == T_OK);              /* the last WRITE */
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && (e->meta.flags & OBJ_DOOM_RECV));
    reap_all();
    CHECK(tstate(40) == THREAD_RUNNABLE && tstate(50) == THREAD_RUNNABLE && ep_ok(e));
    run_user();
    CHECK(test_report_count == 2 && (int64_t)test_reports[0].a == T_EDEAD);
    /* A later receive fails at once (no sender can exist any more). */
    struct cspace c40b = task_cs(40);
    CHECK(endpoint_recv(th(40), &c40b, 1) == T_EDEAD);
    ut_teardown();
}

/* P31, P26: destroying a waiter, and the endpoint itself. */
static void test_ep_destroy(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(ep_thread(10, true, 1, 1, CAP_R_WRITE));
    CHECK(ep_thread(20, true, 1, 2, CAP_R_WRITE));
    CHECK(ep_thread(30, true, 1, 3, CAP_R_WRITE));
    run_user();
    CHECK(cap_delete(&uts, 20 + T_THR) == T_OK);
    CHECK(e->senders.head == th(10) && th(10)->wait_next == th(30) && ep_ok(e));
    for (cap_slot_t b = 10; b <= 30; b += 20) {
        struct cspace c = task_cs(b);
        CHECK(cap_delete(&c, 1) == T_OK);
    }
    CHECK(tstate(10) == THREAD_BLOCKED_SEND);       /* the master (READ) remains */
    CHECK(cap_delete(&uts, 1) == T_OK);             /* the object goes */
    /* Thread 20's cspace still holds a send capability, so this is the
     * loss of the last READ, not death: doomed and listed, waiters
     * intact until the reaper fails them. */
    CHECK(e->meta.flags == (OBJ_LISTED | OBJ_DOOM_SEND) && e->meta.live == 1);
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && ut_store[0].ut.dying == 0);
    reap_all();
    CHECK(tstate(10) == THREAD_RUNNABLE && tstate(30) == THREAD_RUNNABLE && ep_ok(e));
    CHECK(e->meta.flags == 0 && e->meta.live == 1);
    run_user();
    CHECK(test_report_count == 2 && (int64_t)test_reports[1].a == T_EDEAD);
    ut_teardown();
    CHECK(AUDIT());
}

/* P5: an IPC buffer is a writable alias for W^X. */
static void test_ep_ipcbuf_wx(void)
{
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(hello), false, NULL));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(cap_derive(&uts, 10 + T_CODE, 70, CAP_R_ALL, 0) == T_OK);   /* code frame: mapped RX */
    CHECK(thread_bind(&uts, 10 + T_THR, TS_IPCBUF, &uts, 70) == T_EPERM);
    /* The mapped capability itself: its own mapping counts too. */
    CHECK(thread_bind(&uts, 10 + T_THR, TS_IPCBUF, &uts, 10 + T_CODE) == T_EPERM);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 71, 1) == T_OK);
    CHECK(cap_derive(&uts, 71, 72, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive(&uts, 71, 73, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive(&uts, 71, 74, CAP_R_READ | CAP_R_GRANT, 0) == T_OK);
    CHECK(thread_bind(&uts, 10 + T_THR, TS_IPCBUF, &uts, 74) == T_EPERM);  /* no WRITE */
    CHECK(thread_bind(&uts, 10 + T_THR, TS_IPCBUF, &uts, 71) == T_OK);
    CHECK(meta_audit());
    CHECK(vspace_map(&uts, 10 + T_AS, 72, ua(0x404000), RX) == T_EPERM);   /* now an alias */
    CHECK(vspace_map(&uts, 10 + T_AS, 72, ua(0x404000), RW) == T_OK);
    CHECK(vspace_map(&uts, 10 + T_AS, 73, ua(0x405000), RO) == T_OK);
    CHECK(meta_audit());
    CHECK(thread_unbind(&uts, 10 + T_THR, TS_IPCBUF) == T_OK);
    CHECK(meta_audit());
    CHECK(vspace_unmap(&uts, 72) == T_OK);
    CHECK(meta_audit());
    CHECK(vspace_map(&uts, 10 + T_AS, 72, ua(0x404000), RX) == T_OK);      /* alias gone */
    ut_teardown();
}

/* ---- Capability transfer (ADR 0009, step 5.4) ---- */

#define G_ CAP_R_GRANT
#define CAPS(n) ((uint64_t)(n) << 6)

/* P55, authority conservation: a byte-exact copy of every capability
 * record in a set of tables (the test cspace, and each task's cspace and
 * its thread's binding slots). */
#define WORLD_TABLES 24
#define WORLD_MAX    384
struct world {
    unsigned n;
    struct cap *at[WORLD_MAX];
    struct cap v[WORLD_MAX];
    const struct cap *parent[WORLD_MAX];   /* CDT parent at the snapshot */
};
static struct world world_a, world_b;
static struct cspace world_t[WORLD_TABLES];
static unsigned world_nt;

static void world_tables(const cap_slot_t *tasks, unsigned n)
{
    world_nt = 0;
    world_t[world_nt++] = uts;
    for (unsigned i = 0; i < n; i++) {
        KASSERT(world_nt + 2 <= WORLD_TABLES);
        world_t[world_nt++] = task_cs(tasks[i]);
        world_t[world_nt++] = (struct cspace){ th(tasks[i])->bind, TS_COUNT };
    }
}

static void world_take(struct world *w)
{
    memset(w, 0, sizeof(*w));
    for (unsigned t = 0; t < world_nt; t++)
        for (uint32_t i = 0; i < world_t[t].nslots; i++) {
            KASSERT(w->n < WORLD_MAX);                     /* bounded copy */
            w->at[w->n] = &world_t[t].slots[i];
            memcpy(&w->v[w->n], &world_t[t].slots[i], sizeof(struct cap));
            if (world_t[t].slots[i].type != CAP_NULL)
                w->parent[w->n] = cdt_parent_of(&world_t[t].slots[i]);
            w->n++;
        }
}

static bool world_same(const struct world *a, const struct world *b)
{
    return a->n == b->n && memcmp(a->at, b->at, a->n * sizeof(a->at[0])) == 0 &&
           memcmp(a->v, b->v, a->n * sizeof(a->v[0])) == 0;
}

static int in_list(const struct cap *c, struct cap *const *list, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (list[i] == c)
            return (int)i;
    return -1;
}

/* P55, success: exactly the n records dst[i] appeared, each a CDT child of
 * src[i] with its type, object, rights and badge and no mapping state;
 * every other record is unchanged apart from its list links, and keeps
 * exactly the parent it had (ADR 0010: parentage is the tree). */
static bool world_transferred(const struct world *a, const struct world *b,
                              struct cap *const *src, struct cap *const *dst, unsigned n)
{
    static const struct cap zero;
    unsigned found = 0;

    if (a->n != b->n || memcmp(a->at, b->at, a->n * sizeof(a->at[0])) != 0)
        return false;
    for (unsigned i = 0; i < a->n; i++) {
        const struct cap *was = &a->v[i], *now = &b->v[i];
        int k = in_list(a->at[i], dst, n);
        if (k >= 0) {
            const struct cap *s = src[k];
            bool badged = s->type == CAP_ENDPOINT || s->type == CAP_NOTIFICATION;
            found++;
            if (memcmp(was, &zero, sizeof(zero)) != 0)
                return false;                              /* was empty */
            if (now->type != s->type || now->object != s->object ||
                now->rights != s->rights || now->badge != (badged ? s->badge : 0) ||
                now->map_entry || now->slot_flags || now->slot_aux ||
                b->parent[i] != s)
                return false;
            continue;
        }
        struct cap x, y;
        memcpy(&x, was, sizeof(x));
        memcpy(&y, now, sizeof(y));
        memset(&x.open, 0, 2 * sizeof(struct cdt_elem));
        memset(&y.open, 0, 2 * sizeof(struct cdt_elem));
        if (memcmp(&x, &y, sizeof(x)) != 0 || a->parent[i] != b->parent[i])
            return false;
    }
    return found == n;
}

/* A sender at b (sys1) of one word `tag` and the capabilities in its
 * cspace slots `caps`: slot 1 is its endpoint capability (RW, GRANT);
 * slots 2 and 3 hold receive-only and send-only copies with GRANT. */
static bool xfer_sender(cap_slot_t b, uint64_t label, uint64_t tag, const uint32_t *caps,
                        unsigned n)
{
    if (!ep_thread(b, true, label, tag, RW_ | G_))
        return false;
    struct cspace cs = task_cs(b);
    if (cap_derive_to(&uts, 1, &cs, 2, CAP_R_READ | G_, 0) != T_OK ||
        cap_derive_to(&uts, 1, &cs, 3, CAP_R_WRITE | G_, 0) != T_OK)
        return false;
    for (unsigned i = 0; i < n; i++)
        tbuf(b)->caps[i] = caps[i];
    th(b)->regs.rsi = MSG_INFO(label, 1) | CAPS(n);
    return true;
}

/* A receiver at b (sys1) offering `slots` of its cspace. */
static bool xfer_receiver(cap_slot_t b, const uint32_t *slots, unsigned n)
{
    if (!ep_thread(b, false, 0, 0, CAP_R_READ | G_))
        return false;
    for (unsigned i = 0; i < n; i++)
        tbuf(b)->recv_slots[i] = slots[i];
    tbuf(b)->recv_count = n;
    return true;
}

static unsigned reports_with(int64_t a)
{
    unsigned k = 0;
    for (unsigned i = 0; i < test_report_count; i++)
        k += (int64_t)test_reports[i].a == a;
    return k;
}

/* P50-P55: transfers of one and three capabilities, both arrival orders. */
static void test_ep_xfer_basic(void)
{
    /* Receiver first; three capabilities: a badged notification, a frame
     * and the endpoint itself. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &uts, 2, 1) == T_OK);
    CHECK(cap_derive(&uts, 2, 3, W_, 0x40) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(xfer_receiver(40, (const uint32_t[]){ 7, 8, 9 }, 3));
    CHECK(ep_thread(10, true, 5, 0xAB, RW_ | G_));
    struct cspace c10 = task_cs(10), c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 3, &c10, 2, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive_to(&uts, 10 + T_STACK, &c10, 3, RW | G_, 0) == T_OK);
    tbuf(10)->caps[0] = 2;
    tbuf(10)->caps[1] = 3;
    tbuf(10)->caps[2] = 1;
    th(10)->regs.rsi = MSG_INFO(5, 1) | CAPS(3);
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    run_user();
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && th(40)->offer.count == 3 && ep_ok(e));
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    test_report_count = 0;
    CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
    run_user();
    world_take(&world_b);
    struct cap *src3[] = { &c10.slots[2], &c10.slots[3], &c10.slots[1] };
    struct cap *dst3[] = { &c40.slots[7], &c40.slots[8], &c40.slots[9] };
    CHECK(world_transferred(&world_a, &world_b, src3, dst3, 3));
    /* Explicitly: sources unchanged, copies carry the same rights and
     * badge (world_transferred checks this too). */
    CHECK(c10.slots[2].badge == 0x40 && c10.slots[2].rights == W_);
    CHECK(c40.slots[7].badge == 0x40 && c40.slots[7].rights == W_);
    CHECK(c10.slots[1].rights == (RW_ | G_) && c40.slots[9].rights == (RW_ | G_));
    CHECK(c40.slots[8].map_state == 0 && c40.slots[8].map_entry == NULL);
    CHECK(test_report_count == 2 && reports_with(T_OK) == 2);
    CHECK(test_reports[1].b == (MSG_INFO(5, 1) | CAPS(3) | (3ull << 8)));  /* P54 */
    CHECK(got(40) == 0xAB && ep_ok(e) && !e->senders.head && !e->receivers.head);
    /* P27a: counts follow the new records. */
    struct cspace *all[] = { &uts, &c10, &c40 };
    CHECK(endpoint_caps_with(&ut_store[1], CAP_R_READ) == 4 &&
          scan_ep_caps(&ut_store[1], CAP_R_READ, all, 3) == 4);
    CHECK(endpoint_caps_with(&ut_store[1], CAP_R_WRITE) == 3 &&
          scan_ep_caps(&ut_store[1], CAP_R_WRITE, all, 3) == 3);
    /* P25: delegation is revocable, and revocation reaches only copies. */
    CHECK(cap_revoke(&c10, 3, 1000) == T_OK && c40.slots[8].type == CAP_NULL);
    CHECK(cap_revoke(&c10, 1, 1000) == T_OK && c40.slots[9].type == CAP_NULL);
    CHECK(c40.slots[7].type == CAP_NOTIFICATION && c40.slots[1].type == CAP_ENDPOINT);
    CHECK(endpoint_caps_with(&ut_store[1], CAP_R_READ) == 3 && ep_ok(e));
    ut_teardown();

    /* Sender first; one capability, staged with the message. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(xfer_sender(10, 6, 0xCD, (const uint32_t[]){ 5 }, 1));
    c10 = task_cs(10);
    CHECK(cap_derive_to(&uts, 1, &c10, 5, CAP_R_WRITE | G_, 0x77) == T_OK);  /* badged */
    tbuf(10)->caps[1] = tbuf(10)->caps[2] = 0x55;     /* beyond ncaps: never staged */
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && th(10)->msg.caps[0] == 5 && ep_ok(e));
    CHECK(th(10)->msg.caps[1] == 0 && th(10)->msg.caps[2] == 0);
    CHECK(xfer_receiver(40, (const uint32_t[]){ 3 }, 1));
    c10 = task_cs(10);
    c40 = task_cs(40);
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    test_report_count = 0;
    run_user();
    world_take(&world_b);
    struct cap *src1[] = { &c10.slots[5] }, *dst1[] = { &c40.slots[3] };
    CHECK(world_transferred(&world_a, &world_b, src1, dst1, 1));
    CHECK(c10.slots[5].badge == 0x77 && c40.slots[3].badge == 0x77 &&
          c40.slots[3].rights == (CAP_R_WRITE | G_));
    CHECK(test_report_count == 2 && reports_with(T_OK) == 2);
    CHECK(test_reports[0].b == (MSG_INFO(6, 1) | CAPS(1) | (1ull << 8)));
    CHECK(got(40) == 0xCD && ep_ok(e));
    ut_teardown();
}

/* P51, P55: every sender-side refusal, with a receiver waiting, leaves
 * every record, thread and queue byte-for-byte unchanged. */
static void test_ep_xfer_refuse(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(xfer_receiver(40, (const uint32_t[]){ 7, 8, 9 }, 3));
    run_user();
    CHECK(tstate(40) == THREAD_BLOCKED_RECV);
    CHECK(spawn(10, P(runner), false, NULL));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    struct cspace c10 = task_cs(10), c40 = task_cs(40);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 70, 1) == T_OK);
    CHECK(thread_bind(&uts, 10 + T_THR, TS_IPCBUF, &uts, 70) == T_OK);
    struct ipc_buffer *sb = phys_to_virt(cap_obj_paddr(&ut_store[70]));
    CHECK(cap_derive_to(&uts, 1, &c10, 1, RW_ | G_, 0) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &c10, 2, CAP_R_WRITE, 0) == T_OK);     /* no GRANT */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &c10, 3, 1) == T_OK);
    CHECK(cap_derive_to(&c10, 3, &c10, 4, RW, 0) == T_OK);              /* no GRANT */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 12, &c10, 5, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_PAGE_TABLE, 0, &c10, 6, 1) == T_OK);

    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    const struct thread *who[] = { th(10), th(40) };
    struct endpoint e0 = *e;
    snapshot(&snap_a, who, 2);
    static const struct { cap_slot_t ep; uint32_t caps[3]; unsigned n; int rc; } bad[] = {
        { 2, { 3 }, 1, T_EPERM },           /* endpoint without GRANT */
        { 1, { 4 }, 1, T_EPERM },           /* capability without GRANT */
        { 1, { 5 }, 1, T_EINVAL },          /* Untyped */
        { 1, { 6 }, 1, T_EINVAL },          /* PageTable */
        { 1, { 7 }, 1, T_EBADCAP },         /* empty slot */
        { 1, { 99 }, 1, T_ERANGE },         /* out of range */
        { 1, { 3, 3, 7 }, 3, T_EBADCAP },   /* the third is bad: none moves */
        { 1, { 3, 1, 4 }, 3, T_EPERM },
        { 1, { 1, 3, 5 }, 3, T_EINVAL },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        for (unsigned j = 0; j < MSG_MAX_CAPS; j++)
            sb->caps[j] = bad[i].caps[j];
        CHECK(endpoint_send(th(10), &c10, bad[i].ep, MSG_INFO(1, 1) | CAPS(bad[i].n)) ==
              bad[i].rc);
    }
    world_take(&world_b);
    snapshot(&snap_b, who, 2);
    CHECK(world_same(&world_a, &world_b));
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);
    CHECK(ep_ok(e));

    /* The same capability listed twice is two derivations. */
    sb->caps[0] = 3;
    sb->caps[1] = 3;
    sb->caps[2] = 1;
    CHECK(endpoint_send(th(10), &c10, 1, MSG_INFO(1, 1) | CAPS(3)) == T_OK);
    world_take(&world_b);
    struct cap *src[] = { &c10.slots[3], &c10.slots[3], &c10.slots[1] };
    struct cap *dst[] = { &c40.slots[7], &c40.slots[8], &c40.slots[9] };
    CHECK(world_transferred(&world_a, &world_b, src, dst, 3));
    /* Two independent CDT children of one source, not one deduplicated. */
    CHECK(cdt_parent_of(&c40.slots[7]) == &c10.slots[3] &&
          cdt_parent_of(&c40.slots[8]) == &c10.slots[3]);
    CHECK(c40.slots[7].type == CAP_FRAME && c40.slots[8].type == CAP_FRAME);
    CHECK(cap_delete(&c40, 8) == T_OK && c40.slots[7].type == CAP_FRAME);
    CHECK(cap_derive_to(&c10, 3, &c40, 8, CAP_R_ALL, 0) == T_OK);   /* restore */
    CHECK(cap_revoke(&c10, 3, 1000) == T_OK);
    CHECK(c40.slots[7].type == CAP_NULL && c40.slots[8].type == CAP_NULL);
    CHECK(c10.slots[4].type == CAP_NULL);                       /* also a child */
    CHECK(cap_derive_to(&c10, 3, &c10, 4, RW, 0) == T_OK);       /* restore */
    CHECK(tstate(40) == THREAD_RUNNABLE && ep_ok(e));
    run_user();

    /* P25, P51: with nobody waiting, the same refusals come at once, from
     * ring 3: the sender never blocks on capabilities it cannot give. */
    static const uint32_t never[] = { 4, 5, 6, 7, 99 };
    static const int never_rc[] = { T_EPERM, T_EINVAL, T_EINVAL, T_EBADCAP, T_ERANGE };
    for (unsigned i = 0; i < ARRAY_SIZE(never); i++) {
        sb->caps[0] = never[i];
        nreq = 0;
        REQ(10, SYS_SEND, 1, MSG_INFO(1, 1) | CAPS(1));
        struct user_regs r = { .rip = UCODE_VA, .rsp = USTACK_TOP };
        CHECK(thread_write_regs(&uts, 10 + T_THR, &r) == T_OK);
        CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
        world_take(&world_a);
        run_user();
        world_take(&world_b);
        CHECK(res(10, 0) == never_rc[i] && tstate(10) == THREAD_INACTIVE);
        CHECK(world_same(&world_a, &world_b) && !e->senders.head && ep_ok(e));
    }
    ut_teardown();

    /* P50: the receiver's own offer, refused with a sender queued. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(xfer_sender(10, 1, 0x11, (const uint32_t[]){ 2 }, 1));
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND);
    CHECK(xfer_receiver(40, (const uint32_t[]){ 7 }, 1));
    CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK);
    c40 = task_cs(40);
    struct ipc_buffer *rb = tbuf(40);
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    e0 = *e;
    snapshot(&snap_a, who, 2);
    static const struct { uint32_t slots[3], count, reserved; int rc; } offers[] = {
        { { 7, 8, 9 }, 4, 0, T_EINVAL },     /* more than three */
        { { 7 }, 1, 1, T_EINVAL },           /* reserved word set */
        { { 99 }, 1, 0, T_EINVAL },          /* out of range */
        { { 7, 8, 7 }, 3, 0, T_EINVAL },     /* duplicate */
        { { 7, 1 }, 2, 0, T_EBUSY },         /* occupied */
    };
    for (unsigned i = 0; i < ARRAY_SIZE(offers); i++) {
        for (unsigned j = 0; j < MSG_MAX_CAPS; j++)
            rb->recv_slots[j] = offers[i].slots[j];
        rb->recv_count = offers[i].count;
        rb->reserved = offers[i].reserved;
        CHECK(endpoint_recv(th(40), &c40, 1) == offers[i].rc);
    }
    world_take(&world_b);
    snapshot(&snap_b, who, 2);
    CHECK(world_same(&world_a, &world_b));
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);
    CHECK(th(10)->msg.caps[0] == 2 && e->senders.head == th(10) && ep_ok(e));
    rb->reserved = 0;
    run_user();
    ut_teardown();
}

/* P24, P52, P56: a receive slot filled, from ring 3 by another thread of
 * the receiver's task, between the offer and the delivery. */
static void test_ep_xfer_collision(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(xfer_receiver(40, (const uint32_t[]){ 5, 6, 7 }, 3));
    /* B: a second thread sharing the receiver's cspace. */
    CHECK(spawn(50, P(runner), false, NULL));
    CHECK(thread_suspend(&uts, 50 + T_THR) == T_OK);
    CHECK(thread_unbind(&uts, 50 + T_THR, TS_CSPACE) == T_OK);
    CHECK(thread_bind(&uts, 50 + T_THR, TS_CSPACE, &uts, 40 + T_CS) == T_OK);
    nreq = 0;
    REQ(50, SYS_CAP_DERIVE, 1, 6, CAP_R_READ, 0);
    CHECK(xfer_sender(10, 3, 0x33, (const uint32_t[]){ 2, 3, 1 }, 3));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    run_user();                                     /* the offer is captured */
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && th(40)->offer.count == 3);
    CHECK(thread_resume(&uts, 50 + T_THR) == T_OK);
    run_user();                                     /* B fills slot 6 */
    struct cspace c40 = task_cs(40);
    CHECK(res(50, 0) == T_OK && c40.slots[6].type == CAP_ENDPOINT);

    world_tables((const cap_slot_t[]){ 10, 40, 50 }, 3);
    world_take(&world_a);
    const struct thread *who[] = { th(40) };
    struct endpoint e0 = *e;
    snapshot(&snap_a, who, 1);
    test_report_count = 0;
    CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
    run_user();                                     /* the transfer fails whole */
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_EBUSY);
    world_take(&world_b);
    snapshot(&snap_b, who, 1);
    CHECK(world_same(&world_a, &world_b));
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && ep_ok(e));

    /* Only the slots used must be empty: slot 7 occupied, two capabilities
     * go to 5 and 6. */
    CHECK(cap_delete(&c40, 6) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &c40, 7, CAP_R_READ, 0) == T_OK);
    CHECK(xfer_sender(20, 4, 0x44, (const uint32_t[]){ 2, 3 }, 2));
    struct cspace c20 = task_cs(20);
    world_tables((const cap_slot_t[]){ 10, 20, 40, 50 }, 4);
    world_take(&world_a);
    run_user();
    world_take(&world_b);
    struct cap *src[] = { &c20.slots[2], &c20.slots[3] };
    struct cap *dst[] = { &c40.slots[5], &c40.slots[6] };
    CHECK(world_transferred(&world_a, &world_b, src, dst, 2));
    CHECK(got(40) == 0x44 && ep_ok(e));
    ut_teardown();
}

/* P25, P51, P53: a queued sender's message re-validated at delivery. */
static void test_ep_xfer_queued(void)
{
    /* S1's third capability (its own endpoint capability) goes while it
     * waits: S1 fails whole and the receiver takes S2's message. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(xfer_sender(10, 1, 0x10, (const uint32_t[]){ 2, 3, 1 }, 3));
    CHECK(xfer_sender(20, 2, 0x20, (const uint32_t[]){ 3 }, 1));
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && tstate(20) == THREAD_BLOCKED_SEND && ep_ok(e));
    struct cspace c10 = task_cs(10), c20 = task_cs(20);
    CHECK(cap_delete(&c10, 1) == T_OK);
    CHECK(tstate(10) == THREAD_BLOCKED_SEND);       /* P43: not cancelled */
    CHECK(xfer_receiver(40, (const uint32_t[]){ 7, 8, 9 }, 3));
    struct cspace c40 = task_cs(40);
    world_tables((const cap_slot_t[]){ 10, 20, 40 }, 3);
    world_take(&world_a);
    test_report_count = 0;
    run_user();
    world_take(&world_b);
    struct cap *src[] = { &c20.slots[3] }, *dst[] = { &c40.slots[7] };
    CHECK(world_transferred(&world_a, &world_b, src, dst, 1));
    CHECK(got(40) == 0x20 && c40.slots[8].type == CAP_NULL);
    CHECK(test_report_count == 3 && reports_with(T_EBADCAP) == 1 && reports_with(T_OK) == 2);
    CHECK(ep_ok(e) && !e->senders.head && !e->receivers.head);
    ut_teardown();

    /* Too many capabilities for the offer: the sender fails, and the
     * receiver goes on to block with its offer. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(xfer_sender(10, 1, 0x10, (const uint32_t[]){ 2, 3 }, 2));
    run_user();
    CHECK(xfer_receiver(40, (const uint32_t[]){ 7 }, 1));
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    test_report_count = 0;
    run_user();
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b));
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_EINVAL);
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && th(40)->offer.count == 1 &&
          th(40)->offer.slots[0] == 7 && ep_ok(e));

    /* The offer was captured: rewriting the buffer now changes nothing. */
    tbuf(40)->recv_slots[0] = 8;
    tbuf(40)->recv_count = 0;
    CHECK(xfer_sender(20, 2, 0x20, (const uint32_t[]){ 2 }, 1));
    c20 = task_cs(20);
    c40 = task_cs(40);
    world_tables((const cap_slot_t[]){ 10, 20, 40 }, 3);
    world_take(&world_a);
    run_user();
    world_take(&world_b);
    struct cap *src2[] = { &c20.slots[2] }, *dst2[] = { &c40.slots[7] };
    CHECK(world_transferred(&world_a, &world_b, src2, dst2, 1));
    CHECK(got(40) == 0x20 && ep_ok(e));
    ut_teardown();
}

/* P37, P53, I-E2: many queued senders that can no longer be delivered,
 * ahead of deliverable ones. One recv fails each of them exactly once,
 * delivers the first good one, and leaves the rest queued untouched. */
static void test_ep_xfer_drain(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    /* Labels 1..7 identify the senders in their reports (RSI is kept). */
    CHECK(xfer_sender(10, 1, 0x1, (const uint32_t[]){ 2, 3 }, 2));   /* too many */
    CHECK(xfer_sender(20, 2, 0x2, (const uint32_t[]){ 3, 2 }, 2));   /* too many */
    CHECK(xfer_sender(30, 3, 0x3, (const uint32_t[]){ 2 }, 1));      /* cap deleted */
    CHECK(xfer_sender(40, 4, 0x4, (const uint32_t[]){ 3 }, 1));      /* cap deleted */
    CHECK(xfer_sender(50, 5, 0x5, (const uint32_t[]){ 2 }, 1));      /* cap changed */
    CHECK(xfer_sender(60, 6, 0x6, (const uint32_t[]){ 3 }, 1));      /* deliverable */
    CHECK(xfer_sender(70, 7, 0x7, (const uint32_t[]){ 2 }, 1));      /* deliverable */
    run_user();
    for (cap_slot_t b = 10; b <= 70; b += 10)
        CHECK(tstate(b) == THREAD_BLOCKED_SEND);
    struct cspace c30 = task_cs(30), c40 = task_cs(40), c50 = task_cs(50);
    CHECK(cap_delete(&c30, 2) == T_OK && cap_delete(&c40, 3) == T_OK);
    CHECK(cap_delete(&c50, 2) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &c50, 2, CAP_R_READ, 0) == T_OK);  /* no GRANT now */
    struct thread *s7 = th(70);
    struct ipc_msg m7 = s7->msg;

    CHECK(xfer_receiver(80, (const uint32_t[]){ 9 }, 1));
    struct cspace c60 = task_cs(60), c80 = task_cs(80);
    world_tables((const cap_slot_t[]){ 10, 20, 30, 40, 50, 60, 70, 80 }, 8);
    world_take(&world_a);
    test_report_count = 0;
    run_user();
    world_take(&world_b);
    struct cap *src[] = { &c60.slots[3] }, *dst[] = { &c80.slots[9] };
    CHECK(world_transferred(&world_a, &world_b, src, dst, 1));
    CHECK(got(80) == 0x6);

    /* Each failed sender reported exactly one error, with its own reason;
     * the good one succeeded; S7 is still queued, message intact. */
    static const int64_t want[] = { T_EINVAL, T_EINVAL, T_EBADCAP, T_EBADCAP, T_EPERM, T_OK };
    for (unsigned i = 0; i < ARRAY_SIZE(want); i++) {
        unsigned seen = 0;
        for (unsigned k = 0; k < test_report_count; k++)
            /* The sender's own report: its info word, no delivered count. */
            if (MSG_LABEL(test_reports[k].b) == i + 1 && !(test_reports[k].b & 0x300)) {
                seen++;
                CHECK((int64_t)test_reports[k].a == want[i]);
            }
        CHECK(seen == 1);
        CHECK(tstate(10 * (i + 1)) == THREAD_INACTIVE);
    }
    CHECK(test_report_count == 7);                      /* + the receiver */
    CHECK(e->senders.head == s7 && e->senders.tail == s7 && tstate(70) == THREAD_BLOCKED_SEND);
    CHECK(memcmp(&s7->msg, &m7, sizeof(m7)) == 0 && ep_ok(e));

    /* A second recv takes S7; no failed sender is charged again. */
    CHECK(xfer_receiver(90, (const uint32_t[]){ 9 }, 1));
    test_report_count = 0;
    run_user();
    CHECK(test_report_count == 2 && reports_with(T_OK) == 2 && got(90) == 0x7);
    for (unsigned k = 0; k < test_report_count; k++)
        CHECK(MSG_LABEL(test_reports[k].b) == 7 || MSG_LABEL(test_reports[k].b) == 0);
    CHECK(ep_ok(e) && !e->senders.head && !e->receivers.head);
    ut_teardown();
}

/* ---- Call and reply (ADR 0009, step 5.5) ---- */

#define TB_PARK 1   /* SYS_TEST_BLOCK queue (NOTIF kind) that parks a runner */
#define INFO_SKIPPED (1ull << 10)

static uint64_t rsi_out(cap_slot_t b, unsigned i) { return reqs(b)[i].a[5]; }
static uint64_t rdi_out(cap_slot_t b, unsigned i) { return reqs(b)[i].a[4]; }

/* A caller at b (sys1, SYS_CALL) of one word `tag`, with the endpoint at
 * cspace slot 1 (`rights`, `badge`), its data page as IPC buffer, and an
 * offer of `n` slots for the reply. */
static bool ep_caller(cap_slot_t b, uint64_t label, uint64_t tag, cap_rights_t rights,
                      uint64_t badge, const uint32_t *slots, unsigned n)
{
    struct user_regs r = { .r12 = SYS_CALL, .rdi = 1, .rsi = MSG_INFO(label, 1) };
    if (!spawn(b, P(sys1), false, &r))
        return false;
    struct cspace cs = task_cs(b);
    tbuf(b)->words[0] = tag;
    for (unsigned i = 0; i < n; i++)
        tbuf(b)->recv_slots[i] = slots[i];
    tbuf(b)->recv_count = n;
    return cap_derive_to(&uts, 1, &cs, 1, rights, badge) == T_OK &&
           thread_bind(&uts, b + T_THR, TS_IPCBUF, &uts, b + T_DATA) == T_OK;
}

/* A server at b: the request runner, suspended, with its own IPC buffer
 * (a frame at uts slot f) and the endpoint at cspace slot 1 with
 * `rights`. Its requests go in with REQ; go() starts it. */
static struct ipc_buffer *ep_server(cap_slot_t b, cap_slot_t f, cap_rights_t rights)
{
    if (!spawn(b, P(runner), false, NULL) || thread_suspend(&uts, b + T_THR) != T_OK)
        return NULL;
    struct cspace cs = task_cs(b);
    if (cap_retype(&uts, 0, CAP_FRAME, 0, &uts, f, 1) != T_OK ||
        thread_bind(&uts, b + T_THR, TS_IPCBUF, &uts, f) != T_OK ||
        cap_derive_to(&uts, 1, &cs, 1, rights, 0) != T_OK)
        return NULL;
    return phys_to_virt(cap_obj_paddr(&ut_store[f]));
}

static void go(cap_slot_t b)
{
    CHECK(thread_resume(&uts, b + T_THR) == T_OK);
    run_user();
}

/* Release a runner parked with SYS_TEST_BLOCK(TB_PARK), and run. */
static void unpark(cap_slot_t b)
{
    CHECK(th(b)->state == THREAD_BLOCKED_NOTIF);
    thread_wake(th(b), T_OK);
    run_user();
}

/* P57, P58, P66: a call in both orders; the reply authority in between. */
static void test_call_basic(void)
{
    /* The caller first: queued with its message, offer and call flags. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(ep_caller(10, 3, 0x31, CAP_R_WRITE | G_, 0x5A, (const uint32_t[]){ 6 }, 1));
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && th(10)->ipc_flags == (IPC_CALL | IPC_CALL_GRANT));
    CHECK(th(10)->offer.count == 1 && th(10)->offer.slots[0] == 6 && th(10)->msg.badge == 0x5A);
    CHECK(ep_ok(e));
    struct ipc_buffer *sb = ep_server(40, 110, CAP_R_READ | G_);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    REQ(40, SYS_REPLY, MSG_INFO(9, 1));
    go(40);
    /* Between: the server holds the authority, the caller waits on it. */
    CHECK(res(40, 0) == T_OK && rsi_out(40, 0) == MSG_INFO(3, 1) && rdi_out(40, 0) == 0x5A);
    CHECK(sb->words[0] == 0x31);
    CHECK(tstate(10) == THREAD_BLOCKED_REPLY && !runq_contains(th(10)));
    CHECK(th(40)->reply_to == th(10) && th(10)->reply_from == th(40));
    CHECK(th(40)->reply_state == REPLY_PENDING && th(40)->reply_flags == REPLY_CALL_GRANT);
    CHECK(th(10)->msg.info == 0 && th(10)->msg.badge == 0 && th(10)->ipc_flags == 0);
    CHECK(th(10)->offer.count == 1 && ep_ok(e));
    sb->words[0] = 0x99;
    test_report_count = 0;
    unpark(40);
    CHECK(res(40, 2) == T_OK && th(40)->reply_state == REPLY_NONE && !th(40)->reply_to);
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_OK);
    CHECK(test_reports[0].b == MSG_INFO(9, 1) && test_reports[0].c == 0);   /* RDI 0 */
    CHECK(got(10) == 0x99 && th(10)->offer.count == 0 && ep_ok(e));
    ut_teardown();

    /* The server first; the reply carries a capability (call_grant). */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    sb = ep_server(40, 110, CAP_R_READ | G_);
    struct cspace c40 = task_cs(40);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &c40, 2, 1) == T_OK);
    sb->caps[0] = 2;
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_REPLY, MSG_INFO(8, 0) | CAPS(1));
    go(40);
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && ep_ok(e));
    CHECK(ep_caller(10, 4, 0x41, CAP_R_WRITE | G_, 0, (const uint32_t[]){ 6 }, 1));
    struct cspace c10 = task_cs(10);
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    test_report_count = 0;
    run_user();
    world_take(&world_b);
    struct cap *src[] = { &c40.slots[2] }, *dst[] = { &c10.slots[6] };
    CHECK(world_transferred(&world_a, &world_b, src, dst, 1));
    CHECK(res(40, 0) == T_OK && rdi_out(40, 0) == 0 && res(40, 1) == T_OK);
    CHECK(test_report_count == 1 && test_reports[0].b == (MSG_INFO(8, 0) | CAPS(1) | (1ull << 8)));
    CHECK(th(40)->reply_state == REPLY_NONE && ep_ok(e));
    ut_teardown();
}

/* P58: a caller's own offer is checked at the call, in both orders; a bad
 * one is refused at once and changes nothing. */
static void test_call_offer(void)
{
    static const struct { uint32_t slots[3]; unsigned n; int rc; } bad[] = {
        { { 6, 6 }, 2, T_EINVAL }, { { 99 }, 1, T_EINVAL }, { { 1 }, 1, T_EBUSY },
    };
    for (unsigned order = 0; order < 2; order++)
        for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
            CHECK(ut_setup(20));
            CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
            struct endpoint *e = endpoint_of(&ut_store[1]);
            if (order) {                         /* a server waiting */
                CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
                run_user();
                CHECK(tstate(40) == THREAD_BLOCKED_RECV);
            }
            CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE | G_, 0, bad[i].slots, bad[i].n));
            struct endpoint e0 = *e;
            run_user();
            CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == bad[i].rc);
            CHECK(memcmp(&e0, e, sizeof(e0)) == 0 && ep_ok(e));
            CHECK(!order || (tstate(40) == THREAD_BLOCKED_RECV && th(40)->reply_state == REPLY_NONE));
            ut_teardown();
        }
}

/* P23a, P61, P65: refused replies keep the authority and change nothing. */
static void test_reply_refuse(void)
{
    /* A caller without GRANT: no capability can come back, whatever the
     * server's own endpoint capability holds. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE, 0, (const uint32_t[]){ 6 }, 1));
    struct ipc_buffer *sb = ep_server(40, 110, CAP_R_ALL);
    struct cspace c40 = task_cs(40), c10 = task_cs(10);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &c40, 2, 1) == T_OK);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    run_user();
    go(40);
    CHECK(th(40)->reply_state == REPLY_PENDING && th(40)->reply_flags == 0);
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    const struct thread *who[] = { th(10), th(40) };
    struct endpoint e0 = *e;
    snapshot(&snap_a, who, 2);
    sb->caps[0] = 2;
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0) | CAPS(1)) == T_EPERM);   /* P23a */
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 33)) == T_EINVAL);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 1) | (1u << 9)) == T_EINVAL);
    world_take(&world_b);
    snapshot(&snap_b, who, 2);
    CHECK(world_same(&world_a, &world_b));
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);
    /* The authority survived: a plain reply still works, exactly once. */
    test_report_count = 0;
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(2, 0)) == T_OK);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(2, 0)) == T_EINVAL);   /* single use */
    CHECK(c10.slots[6].type == CAP_NULL && tstate(10) == THREAD_RUNNABLE);
    unpark(40);
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_OK);
    CHECK(test_reports[0].b == MSG_INFO(2, 0) && ep_ok(e));
    ut_teardown();

    /* The same with the server waiting first: call_grant comes from the
     * caller's capability in this order too. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    sb = ep_server(40, 110, CAP_R_ALL);
    c40 = task_cs(40);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &c40, 2, 1) == T_OK);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    go(40);
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE, 0, (const uint32_t[]){ 6 }, 1));
    run_user();
    CHECK(th(40)->reply_state == REPLY_PENDING && th(40)->reply_flags == 0);
    sb->caps[0] = 2;
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0) | CAPS(1)) == T_EPERM);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0)) == T_OK);
    unpark(40);
    ut_teardown();

    /* A caller with GRANT whose side no longer fits. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE | G_, 0, (const uint32_t[]){ 6 }, 1));
    sb = ep_server(40, 110, CAP_R_READ);
    c40 = task_cs(40);
    c10 = task_cs(10);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &c40, 2, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &c40, 3, 1) == T_OK);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    run_user();
    go(40);
    CHECK(th(40)->reply_state == REPLY_PENDING && th(40)->reply_flags == REPLY_CALL_GRANT);
    CHECK(cap_derive_to(&uts, 1, &c10, 6, CAP_R_READ, 0) == T_OK);   /* filled since */
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    e0 = *e;
    snapshot(&snap_a, who, 2);
    sb->caps[0] = 2;
    sb->caps[1] = 3;
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0) | CAPS(1)) == T_EBUSY);   /* P24 */
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0) | CAPS(2)) == T_EINVAL);  /* too many */
    sb->caps[0] = 9;
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0) | CAPS(1)) == T_EBADCAP);
    world_take(&world_b);
    snapshot(&snap_b, who, 2);
    CHECK(world_same(&world_a, &world_b));
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);
    /* P6a: the caller may lose its buffer while waiting; then data cannot
     * reach it, but a label-only reply can. */
    CHECK(thread_unbind(&uts, 10 + T_THR, TS_IPCBUF) == T_OK);
    CHECK(tstate(10) == THREAD_BLOCKED_REPLY);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 1)) == T_EINVAL);
    CHECK(th(40)->reply_state == REPLY_PENDING);
    test_report_count = 0;
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(3, 0)) == T_OK);
    unpark(40);
    CHECK(test_report_count == 1 && test_reports[0].b == MSG_INFO(3, 0) && ep_ok(e));
    ut_teardown();
}

/* P28, P29, P62, P64: the caller or the server goes away. */
static void test_reply_gone(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    struct ipc_buffer *sb = ep_server(40, 110, CAP_R_READ);
    (void)sb;
    struct cspace c40 = task_cs(40);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);

    /* Caller suspended while waiting: the authority is marked gone; the
     * next reply consumes it with T_EDEAD; the caller's call: T_EINTR. */
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE, 0, NULL, 0));
    run_user();
    go(40);
    CHECK(tstate(10) == THREAD_BLOCKED_REPLY && th(40)->reply_state == REPLY_PENDING);
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(th(40)->reply_state == REPLY_CALLER_GONE && !th(40)->reply_to && ep_ok(e));
    CHECK(th(40)->reply_flags == 0 && th(10)->offer.count == 0);
    CHECK(endpoint_recv(th(40), &c40, 1) == T_EBUSY);    /* still held */
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0)) == T_EDEAD);
    CHECK(th(40)->reply_state == REPLY_NONE);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0)) == T_EINVAL);
    CHECK(th(10)->regs.rax == (uint64_t)(int64_t)T_EINTR);

    /* Caller destroyed while waiting: no link to it remains. */
    CHECK(ep_caller(20, 2, 0x22, CAP_R_WRITE, 0, NULL, 0));
    run_user();
    unpark(40);
    CHECK(th(40)->reply_to == th(20) && tstate(20) == THREAD_BLOCKED_REPLY);
    CHECK(cap_delete(&uts, 20 + T_THR) == T_OK);
    CHECK(th(40)->reply_state == REPLY_CALLER_GONE && !th(40)->reply_to && ep_ok(e));
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0)) == T_EDEAD);
    ut_teardown();

    /* The server suspended, destroyed or exiting with the authority: its
     * caller is woken with T_EDEAD. */
    for (unsigned how = 0; how < 3; how++) {
        CHECK(ut_setup(20));
        CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
        e = endpoint_of(&ut_store[1]);
        CHECK(ep_server(40, 110, CAP_R_READ) != NULL);
        nreq = 0;
        REQ(40, SYS_RECV, 1);
        if (how != 2)
            REQ(40, SYS_TEST_BLOCK, TB_PARK);   /* else: exits holding it */
        CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE, 0, NULL, 0));
        CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
        go(40);
        test_report_count = 0;
        go(10);
        if (how == 0)
            CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK);
        else if (how == 1)
            CHECK(cap_delete(&uts, 40 + T_THR) == T_OK);
        if (how != 2) {
            CHECK(tstate(10) == THREAD_RUNNABLE);
            run_user();
        }
        CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_EDEAD);
        CHECK(th(10)->reply_from == NULL && ep_ok(e));
        if (how != 1)
            CHECK(th(40)->reply_state == REPLY_NONE && !th(40)->reply_to);
        ut_teardown();
    }

    /* A caller aborted while still queued loses its staged call. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE | G_, 0, (const uint32_t[]){ 6, 7 }, 2));
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && th(10)->offer.count == 2);
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(th(10)->offer.count == 0 && th(10)->ipc_flags == 0 && th(10)->msg.info == 0);
    CHECK(th(10)->regs.rax == (uint64_t)(int64_t)T_EINTR && ep_ok(e) && !e->senders.head);

    /* P62: a thread without authority cannot answer anyone's caller. */
    CHECK(ep_server(40, 110, CAP_R_READ) != NULL);
    c40 = task_cs(40);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    CHECK(ep_caller(20, 2, 0x22, CAP_R_WRITE, 0, NULL, 0));
    run_user();
    go(40);
    CHECK(ep_server(50, 111, CAP_R_READ) != NULL);
    struct cspace c50 = task_cs(50);
    CHECK(endpoint_reply(th(50), &c50, MSG_INFO(1, 0)) == T_EINVAL);
    CHECK(tstate(20) == THREAD_BLOCKED_REPLY && th(20)->reply_from == th(40) && ep_ok(e));
    ut_teardown();
}

/* P63: reply_recv, both halves validated before either commits. */
static void test_reply_recv(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    struct ipc_buffer *sb = ep_server(40, 110, CAP_R_READ | G_);
    struct cspace c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 1, &c40, 3, CAP_R_WRITE, 0) == T_OK);   /* no READ */
    sb->recv_count = 1;
    sb->recv_slots[0] = 7;
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    REQ(40, SYS_REPLY_RECV, 1, MSG_INFO(5, 1));
    REQ(40, SYS_REPLY, MSG_INFO(6, 0));
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE | G_, 0x10, NULL, 0));
    run_user();
    go(40);
    CHECK(th(40)->reply_to == th(10));
    /* Queued behind: an undeliverable caller (two capabilities for a
     * one-slot offer), then a good one. */
    CHECK(xfer_sender(30, 3, 0x33, (const uint32_t[]){ 2, 3 }, 2));
    th(30)->regs.r12 = SYS_CALL;
    CHECK(ep_caller(20, 2, 0x22, CAP_R_WRITE | G_, 0x20, NULL, 0));
    run_user();
    CHECK(tstate(30) == THREAD_BLOCKED_SEND && tstate(20) == THREAD_BLOCKED_SEND);
    CHECK(e->senders.head == th(30));

    /* Refusals: the receive half, then the reply half. Nothing changes,
     * not even the undeliverable caller (no drain before both pass). */
    world_tables((const cap_slot_t[]){ 10, 20, 30, 40 }, 4);
    world_take(&world_a);
    const struct thread *who[] = { th(10), th(20), th(30), th(40) };
    struct endpoint e0 = *e;
    snapshot(&snap_a, who, 4);
    CHECK(endpoint_reply_recv(th(40), &c40, 3, MSG_INFO(5, 0)) == T_EPERM);   /* no READ */
    sb->recv_count = 2;
    sb->recv_slots[1] = 7;
    CHECK(endpoint_reply_recv(th(40), &c40, 1, MSG_INFO(5, 0)) == T_EINVAL);  /* duplicate */
    sb->recv_count = 1;
    CHECK(endpoint_reply_recv(th(40), &c40, 1, MSG_INFO(5, 33)) == T_EINVAL); /* reply */
    sb->caps[0] = 1;                                   /* has GRANT; caller offers 0 */
    CHECK(endpoint_reply_recv(th(40), &c40, 1, MSG_INFO(5, 0) | CAPS(1)) == T_EINVAL);
    world_take(&world_b);
    snapshot(&snap_b, who, 4);
    CHECK(world_same(&world_a, &world_b));
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0 && memcmp(&e0, e, sizeof(e0)) == 0);

    /* Success: the reply goes to 10, the bad caller is failed, and the
     * server now holds the authority for 20. */
    sb->words[0] = 0x55;
    test_report_count = 0;
    unpark(40);
    CHECK(res(40, 2) == T_OK && rsi_out(40, 2) == MSG_INFO(2, 1) && rdi_out(40, 2) == 0x20);
    CHECK(res(40, 3) == T_OK && tstate(20) == THREAD_INACTIVE);
    CHECK(test_report_count == 3 && reports_with(T_OK) == 2 && reports_with(T_EINVAL) == 1);
    CHECK(got(10) == 0x55 && ep_ok(e) && !e->senders.head);
    ut_teardown();
}

/* P63: liveness and the shared-cspace slot rule; caller-gone bit 10. */
static void test_reply_recv_edges(void)
{
    /* No sender can ever come: T_EDEAD, and the authority is kept. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(ep_server(40, 110, CAP_R_READ) != NULL);
    struct cspace c40 = task_cs(40);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE, 0, NULL, 0));
    run_user();
    go(40);
    struct cspace c10 = task_cs(10);
    CHECK(cap_delete(&c10, 1) == T_OK && cap_delete(&uts, 1) == T_OK);
    CHECK(endpoint_caps_with(&c40.slots[1], CAP_R_WRITE) == 0);
    const struct thread *who[] = { th(10), th(40) };
    snapshot(&snap_a, who, 2);
    CHECK(endpoint_reply_recv(th(40), &c40, 1, MSG_INFO(1, 0)) == T_EDEAD);
    snapshot(&snap_b, who, 2);
    CHECK(memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(1, 0)) == T_OK && ep_ok(e));
    unpark(40);
    ut_teardown();

    /* Caller and server share a cspace: the reply may not fill a slot the
     * server's own receive has offered. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE | G_, 0, (const uint32_t[]){ 6 }, 1));
    struct ipc_buffer *sb = ep_server(50, 110, CAP_R_READ);
    CHECK(thread_unbind(&uts, 50 + T_THR, TS_CSPACE) == T_OK);
    CHECK(thread_bind(&uts, 50 + T_THR, TS_CSPACE, &uts, 10 + T_CS) == T_OK);
    c10 = task_cs(10);
    CHECK(cap_derive_to(&uts, 1, &c10, 2, CAP_R_READ | G_, 0) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &c10, 3, 1) == T_OK);
    sb->recv_count = 1;
    sb->recv_slots[0] = 6;
    sb->caps[0] = 3;
    nreq = 0;
    REQ(50, SYS_RECV, 2);
    REQ(50, SYS_TEST_BLOCK, TB_PARK);
    REQ(50, SYS_REPLY_RECV, 2, MSG_INFO(1, 0) | CAPS(1));
    run_user();
    go(50);
    CHECK(th(50)->reply_to == th(10) && c10.slots[6].type == CAP_NULL);
    world_tables((const cap_slot_t[]){ 10, 50 }, 2);
    world_take(&world_a);
    const struct thread *who2[] = { th(10), th(50) };
    snapshot(&snap_a, who2, 2);
    CHECK(endpoint_reply_recv(th(50), &c10, 2, MSG_INFO(1, 0) | CAPS(1)) == T_EBUSY);
    world_take(&world_b);
    snapshot(&snap_b, who2, 2);
    CHECK(world_same(&world_a, &world_b) && memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0);
    /* With disjoint slots it goes through and the server blocks. */
    sb->recv_slots[0] = 7;
    world_take(&world_a);
    unpark(50);
    world_take(&world_b);
    struct cap *src[] = { &c10.slots[3] }, *dst[] = { &c10.slots[6] };
    CHECK(world_transferred(&world_a, &world_b, src, dst, 1));
    CHECK(tstate(50) == THREAD_BLOCKED_RECV && th(50)->offer.slots[0] == 7 && ep_ok(e));
    ut_teardown();

    /* A caller-gone authority is consumed by reply_recv; bit 10 arrives
     * with the receive that completes, even after it blocked. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(ep_server(40, 110, CAP_R_READ) != NULL);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    REQ(40, SYS_REPLY_RECV, 1, MSG_INFO(1, 0));
    REQ(40, SYS_REPLY, MSG_INFO(1, 0));
    CHECK(ep_caller(10, 1, 0x11, CAP_R_WRITE, 0, NULL, 0));
    run_user();
    go(40);
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(th(40)->reply_state == REPLY_CALLER_GONE);
    unpark(40);
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && th(40)->ipc_flags == IPC_REPLY_SKIPPED);
    CHECK(th(40)->reply_state == REPLY_NONE && ep_ok(e));
    CHECK(ep_caller(20, 2, 0x22, CAP_R_WRITE, 0x77, NULL, 0));
    test_report_count = 0;
    run_user();
    CHECK(res(40, 2) == T_OK && rsi_out(40, 2) == (MSG_INFO(2, 1) | INFO_SKIPPED));
    CHECK(rdi_out(40, 2) == 0x77 && res(40, 3) == T_OK && th(40)->ipc_flags == 0);
    CHECK(test_report_count == 1 && (int64_t)test_reports[0].a == T_OK && ep_ok(e));
    ut_teardown();
}

/* P57: the badge of the capability used, captured at the send. */
static void test_ep_badge(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    struct user_regs r = { .r12 = SYS_SEND, .rdi = 1, .rsi = MSG_INFO(1, 0) };
    CHECK(spawn(10, P(sys1), false, &r));
    struct cspace c10 = task_cs(10);
    CHECK(cap_derive_to(&uts, 1, &c10, 1, CAP_R_WRITE, 0xB1) == T_OK);
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && th(10)->msg.badge == 0xB1);
    CHECK(cap_delete(&c10, 1) == T_OK);              /* the capability goes */
    CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
    test_report_count = 0;
    run_user();
    /* The receiver's report: RAX, RSI (info), RDI (badge). */
    bool seen = false;
    for (unsigned i = 0; i < test_report_count; i++)
        if (test_reports[i].b == MSG_INFO(1, 0) && test_reports[i].c == 0xB1)
            seen = true;
    CHECK(seen && ep_ok(e));
    /* Unbadged: 0. */
    CHECK(ep_thread(20, true, 2, 0x2, CAP_R_WRITE));
    CHECK(ep_thread(50, false, 0, 0, CAP_R_READ));
    test_report_count = 0;
    run_user();
    /* The sender's own report keeps its RDI (slot 1); the receiver's is 0. */
    CHECK(test_report_count == 2 && test_reports[0].c + test_reports[1].c == 1);
    ut_teardown();
}

/* ---- Fault IPC (ADR 0009, step 5.6) ---- */

#define FAULT_INFO (((uint64_t)FAULT_LABEL << 32) | FAULT_WORDS | INFO_FAULT)

/* Bind thread b's TS_FAULT to a copy of the endpoint at uts `ep` with
 * `rights` (+GRANT, which binding needs) and `badge`. The copy at uts slot
 * b + 9 is the binding's parent. */
static bool fault_bind(cap_slot_t b, cap_slot_t ep, cap_rights_t rights, uint64_t badge)
{
    return cap_derive(&uts, ep, b + 9, rights | G_, badge) == T_OK &&
           thread_bind(&uts, b + T_THR, TS_FAULT, &uts, b + 9) == T_OK;
}

/* A server parked holding a fault: ep_server at b, receiving on its slot
 * 1, then parked. */
static struct ipc_buffer *fault_handler(cap_slot_t b, cap_slot_t f)
{
    struct ipc_buffer *hb = ep_server(b, f, CAP_R_READ);
    nreq = 0;
    REQ(b, SYS_RECV, 1);
    REQ(b, SYS_TEST_BLOCK, TB_PARK);
    return hb;
}

/* P68-P71, P73: the frozen message, then a label-0 reply that resumes the
 * thread at the faulting instruction with no register written. */
static void test_fault_deliver(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    struct ipc_buffer *hb = fault_handler(40, 110);
    go(40);                                           /* the handler waits */
    /* Distinctive registers: an RSP no other thread has, and an RAX the
     * fault path must leave alone (P70). */
    struct user_regs ir = { .rax = 0x5EED, .rsp = USTACK_TOP - 0x48 };
    CHECK(spawn(10, P(f_pf_retry), false, &ir));
    CHECK(th(10)->regs.rsp == USTACK_TOP);   /* spawn sets the stack top... */
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    ir = th(10)->regs;
    ir.rsp = USTACK_TOP - 0x48;              /* ...so move it */
    CHECK(thread_write_regs(&uts, 10 + T_THR, &ir) == T_OK);
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0xF1));
    go(10);
    CHECK(res(40, 0) == T_OK && rsi_out(40, 0) == FAULT_INFO && rdi_out(40, 0) == 0xF1);
    CHECK(hb->words[0] == 14 && hb->words[1] == 4 && hb->words[2] == 0x500A08);
    CHECK(hb->words[3] == UCODE_VA && hb->words[4] == USTACK_TOP - 0x48);
    CHECK(th(10)->regs.rax == 0x5EED);
    CHECK(hb->words[5] == (th(10)->regs.rflags & RFLAGS_USER_MASK) && hb->words[6] == 0);
    CHECK(tstate(10) == THREAD_BLOCKED_REPLY && th(40)->reply_flags == REPLY_FAULT);
    struct fault_record f = tfault(10);
    CHECK(f.vector == 14 && f.error == 4 && f.addr == 0x500A08 && f.rip == UCODE_VA);
    CHECK(th(10)->offer.count == 0 && ep_ok(e));

    /* The handler's side: map the page, then reply label 0. */
    struct user_regs before = th(10)->regs;
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 111, 1) == T_OK);
    *(uint64_t *)((uint8_t *)phys_to_virt(cap_obj_paddr(&ut_store[111])) + 0xA08) = 0x7777;
    CHECK(vspace_map(&uts, 10 + T_AS, 111, ua(0x500000), RO) == T_OK);
    struct cspace c40 = task_cs(40);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(0, 0)) == T_OK);
    CHECK(tstate(10) == THREAD_RUNNABLE && th(40)->reply_state == REPLY_NONE);
    CHECK(memcmp(&before, &th(10)->regs, sizeof(before)) == 0);        /* P70 */
    CHECK(th(10)->ret_path == RETURN_IRET);
    f = tfault(10);
    CHECK(f.vector == 0 && f.error == 0 && f.addr == 0 && f.rip == 0); /* P73 */
    run_user();
    CHECK(test_report_count == 1 && test_reports[0].a == 0x7777);      /* retried */
    unpark(40);
    CHECK(ep_ok(e));
    ut_teardown();
}

/* P33, P75: a ring-3 handler changes the thread only through its Thread
 * capability; without it, it can only resume or leave it faulted. */
static void test_fault_authority(void)
{
    /* With the Thread capability: skip the ud2. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(spawn(10, P(f_ud_go), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0x10));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(ep_server(40, 110, CAP_R_READ) != NULL);
    struct cspace c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 10 + T_THR, &c40, 2, CAP_R_ALL, 0) == T_OK);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TH_READ_REGS, 2, UBUF);
    REQ(40, SYS_TEST_BLOCK, TB_PARK);
    REQ(40, SYS_TH_WRITE_REGS, 2, UBUF);
    REQ(40, SYS_REPLY, MSG_INFO(0, 0));
    go(40);
    go(10);
    CHECK(res(40, 1) == T_OK && tstate(10) == THREAD_BLOCKED_REPLY);
    struct user_regs *ur = ubuf(40, UBUF);
    CHECK(ur->rip == UCODE_VA);
    ur->rip += 2;                                     /* the handler's decision */
    unpark(40);
    CHECK(res(40, 3) == T_OK && res(40, 4) == T_OK);
    CHECK(test_report_count == 1 && test_reports[0].a == 0x600D && ep_ok(e));
    ut_teardown();

    /* Without it: no register change, no suspension; label 0 retries the
     * same instruction (a second, identical fault); label 1 leaves it
     * FAULTED with its record. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(spawn(10, P(f_ud_go), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0x10));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    struct ipc_buffer *hb = ep_server(40, 110, CAP_R_READ);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_TH_WRITE_REGS, 2, UBUF);
    REQ(40, SYS_TH_SUSPEND, 2);
    REQ(40, SYS_TH_READ_REGS, 1, UBUF);              /* the endpoint is no Thread cap */
    REQ(40, SYS_REPLY, MSG_INFO(0, 0));
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_REPLY, MSG_INFO(1, 0));
    go(40);
    go(10);
    CHECK(res(40, 1) == T_EBADCAP && res(40, 2) == T_EBADCAP && res(40, 3) == T_EBADCAP);
    CHECK(res(40, 4) == T_OK && res(40, 5) == T_OK && res(40, 6) == T_OK);
    CHECK(rsi_out(40, 5) == FAULT_INFO && hb->words[0] == 6 && hb->words[3] == UCODE_VA);
    CHECK(tstate(10) == THREAD_FAULTED && tfault(10).vector == 6 && tfault(10).rip == UCODE_VA);
    CHECK(test_report_count == 0 && ep_ok(e));
    ut_teardown();
}

/* P34, P69, P73: no usable handler. */
static void test_fault_nohandler(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    /* A binding without WRITE is no handler. */
    CHECK(spawn(10, P(f_ud), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_READ, 0));
    run_user();
    CHECK(tstate(10) == THREAD_FAULTED && tfault(10).vector == 6 && !e->senders.head);
    /* Resuming clears the record; the thread faults again and gets a new
     * one (P73). */
    CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
    CHECK(tfault(10).vector == 0 && tfault(10).rip == 0);
    run_user();
    CHECK(tstate(10) == THREAD_FAULTED && tfault(10).vector == 6 && tfault(10).rip == UCODE_VA);
    /* Nobody can ever receive: FAULTED at once, never queued. */
    CHECK(spawn(20, P(f_ud), false, NULL));
    CHECK(fault_bind(20, 1, CAP_R_WRITE, 0));
    CHECK(cap_delete(&uts, 1) == T_OK && cap_delete(&uts, 10 + 9) == T_OK);
    CHECK(thread_unbind(&uts, 10 + T_THR, TS_FAULT) == T_OK);   /* a binding counts too */
    CHECK(endpoint_caps_with(&ut_store[29], CAP_R_READ) == 0);
    run_user();
    CHECK(tstate(20) == THREAD_FAULTED && !e->senders.head && ep_ok(e));
    ut_teardown();
}

/* P72: the handler or the endpoint goes away; the faulting thread goes
 * FAULTED with no register written. Suspension of the faulting thread
 * itself makes it re-execute the fault later. */
static void test_fault_handler_gone(void)
{
    for (unsigned how = 0; how < 5; how++) {
        CHECK(ut_setup(20));
        CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
        struct endpoint *e = endpoint_of(&ut_store[1]);
        CHECK(ep_server(40, 110, CAP_R_READ) != NULL);   /* holds READ; idle */
        nreq = 0;
        REQ(40, SYS_RECV, 1);
        if (how < 2)
            REQ(40, SYS_TEST_BLOCK, TB_PARK);         /* how 2: exits instead */
        if (how < 3)
            go(40);                                 /* how 2: receives, then exits */
        CHECK(spawn(10, P(f_ud), false, NULL));
        CHECK(fault_bind(10, 1, CAP_R_WRITE, 0));
        run_user();
        struct user_regs before = th(10)->regs;
        switch (how) {
        case 0:                                     /* handler suspended */
            CHECK(tstate(10) == THREAD_BLOCKED_REPLY);
            CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK);
            break;
        case 1:                                     /* handler destroyed */
            CHECK(tstate(10) == THREAD_BLOCKED_REPLY);
            CHECK(cap_delete(&uts, 40 + T_THR) == T_OK);
            break;
        case 2:                                     /* handler exited */
            break;
        case 3: {                                   /* queued; last READ gone */
            CHECK(tstate(10) == THREAD_BLOCKED_SEND && th(10)->ipc_flags == (IPC_CALL | IPC_FAULT));
            struct cspace c40 = task_cs(40);
            CHECK(cap_delete(&c40, 1) == T_OK && cap_delete(&uts, 1) == T_OK);
            break;
        }
        case 4:                                     /* queued; endpoint destroyed */
            CHECK(tstate(10) == THREAD_BLOCKED_SEND);
            CHECK(cap_revoke(&uts, 1, 1000) == T_OK && cap_delete(&uts, 1) == T_OK);
            break;
        }
        reap_all();   /* cases 3 and 4: the reaper fails the queued fault call */
        CHECK(tstate(10) == THREAD_FAULTED && tfault(10).vector == 6);
        CHECK(memcmp(&before, &th(10)->regs, sizeof(before)) == 0);
        CHECK(th(10)->regs.rax == 0 && th(10)->regs.rip == UCODE_VA);   /* no T_EDEAD */
        CHECK(!runq_contains(th(10)) && th(10)->reply_from == NULL && AUDIT());
        if (how != 4)
            CHECK(ep_ok(e));
        ut_teardown();
    }

    /* The faulting thread suspended while queued: INACTIVE, registers
     * untouched (no T_EINTR); resumed, it faults again and re-queues. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(spawn(10, P(f_ud), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0));
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND);
    struct user_regs before = th(10)->regs;
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(tstate(10) == THREAD_INACTIVE && th(10)->ipc_flags == 0 && th(10)->msg.info == 0);
    CHECK(memcmp(&before, &th(10)->regs, sizeof(before)) == 0 && !e->senders.head);
    CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
    run_user();
    CHECK(tstate(10) == THREAD_BLOCKED_SEND && th(10)->msg.words[3] == UCODE_VA && ep_ok(e));
    ut_teardown();
}

/* P74: a handler whose fault endpoint is the one it serves. Its fault
 * queues there; it is never its own receiver; nothing recurses; its reply
 * authority is kept until a supervisor stops it. */
static void test_fault_self(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(fault_handler(40, 110) != NULL);
    CHECK(fault_bind(40, 1, CAP_R_WRITE, 0xEE));      /* its own endpoint */
    go(40);
    CHECK(spawn(10, P(f_ud), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0x07));
    run_user();
    CHECK(th(40)->reply_to == th(10) && tstate(10) == THREAD_BLOCKED_REPLY);
    /* The handler faults (a jump to an unmapped address). */
    struct user_regs r = th(40)->regs;
    r.rip = 0x10;
    CHECK(thread_write_regs(&uts, 40 + T_THR, &r) == T_OK);
    unpark(40);
    CHECK(tstate(40) == THREAD_BLOCKED_SEND && th(40)->ipc_flags == (IPC_CALL | IPC_FAULT));
    CHECK(e->senders.head == th(40) && !e->receivers.head);
    CHECK(th(40)->msg.badge == 0xEE && th(40)->msg.words[3] == 0x10);
    CHECK(th(40)->reply_state == REPLY_PENDING && th(40)->reply_to == th(10));   /* kept */
    CHECK(tstate(10) == THREAD_BLOCKED_REPLY && ep_ok(e));
    /* A supervisor stops it: terminal, so its authority dies; the thread
     * it was handling was delivering a fault, so it becomes FAULTED. */
    struct user_regs before = th(10)->regs;
    CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK);
    CHECK(tstate(40) == THREAD_INACTIVE && th(40)->regs.rip == 0x10);
    CHECK(tstate(10) == THREAD_FAULTED && memcmp(&before, &th(10)->regs, sizeof(before)) == 0);
    CHECK(ep_ok(e) && !e->senders.head);
    ut_teardown();
}

/* P64, P74: a handler that faults while holding a fault reply, handled by
 * a supervisor; leaving it FAULTED does not cascade. */
static void test_fault_nested(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 2, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]), *e2 = endpoint_of(&ut_store[2]);
    CHECK(fault_handler(40, 110) != NULL);            /* H serves E */
    CHECK(fault_bind(40, 2, CAP_R_WRITE, 0x44));      /* H's faults go to E2 */
    struct ipc_buffer *sb = ep_server(50, 111, CAP_R_READ);   /* S serves E2 */
    struct cspace c50 = task_cs(50);
    CHECK(cap_derive_to(&uts, 2, &c50, 3, CAP_R_READ, 0) == T_OK);
    nreq = 0;
    REQ(50, SYS_RECV, 3);
    REQ(50, SYS_TEST_BLOCK, TB_PARK);
    go(40);
    go(50);
    CHECK(spawn(10, P(f_ud), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0x07));
    run_user();
    CHECK(th(40)->reply_to == th(10));
    struct user_regs r = th(40)->regs;
    r.rip = 0x10;
    CHECK(thread_write_regs(&uts, 40 + T_THR, &r) == T_OK);
    unpark(40);
    CHECK(rsi_out(50, 0) == FAULT_INFO && rdi_out(50, 0) == 0x44 && sb->words[3] == 0x10);
    CHECK(tstate(40) == THREAD_BLOCKED_REPLY && th(50)->reply_to == th(40));
    CHECK(th(40)->reply_to == th(10) && tstate(10) == THREAD_BLOCKED_REPLY);
    /* S leaves H faulted: H keeps the authority for T (FAULTED is not
     * terminal); T keeps waiting. No cascade. */
    CHECK(endpoint_reply(th(50), &c50, MSG_INFO(1, 0)) == T_OK);
    CHECK(tstate(40) == THREAD_FAULTED && tfault(40).rip == 0x10);
    CHECK(th(40)->reply_state == REPLY_PENDING && tstate(10) == THREAD_BLOCKED_REPLY);
    CHECK(ep_ok(e) && AUDIT());
    (void)e2;
    /* Stopping H is terminal: T becomes FAULTED. */
    CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK);
    CHECK(tstate(10) == THREAD_FAULTED && th(40)->reply_state == REPLY_NONE && AUDIT());
    unpark(50);
    ut_teardown();
}

/* P67: faults in kernel mode on a thread's behalf never become fault
 * messages; IPC never touches user virtual addresses. */
static void test_fault_kernel_paths(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(ep_server(40, 110, CAP_R_READ) != NULL);
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    go(40);                                           /* a handler waits */
    CHECK(spawn(10, P(runner), false, NULL));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK);
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0));
    struct cspace c10 = task_cs(10);
    CHECK(cap_install(&c10, 3, CAP_DEBUG_CONSOLE, CAP_R_WRITE, 0) == T_OK);
    CHECK(cap_derive_to(&uts, 10 + T_THR, &c10, 4, CAP_R_ALL, 0) == T_OK);
    nreq = 0;
    REQ(10, SYS_DEBUG_WRITE, 3, 0x10, 8);                     /* copy_from_user */
    REQ(10, SYS_DEBUG_WRITE, 3, 0xFFFF800000000000ull, 8);
    REQ(10, SYS_TH_READ_REGS, 4, 0x10);                       /* copy_to_user */
    go(10);
    CHECK(res(10, 0) == T_EFAULT && res(10, 1) == T_EFAULT && res(10, 2) == T_EFAULT);
    CHECK(tstate(10) == THREAD_INACTIVE && tstate(40) == THREAD_BLOCKED_RECV);   /* no message */
    CHECK(tfault(10).vector == 0 && ep_ok(e));
    ut_teardown();
}

/* P68, P69: trap boundary, the thread's own kernel-half address, no
 * capability or data in a fault reply, and an unforgeable fault bit. */
static void test_fault_contract(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    struct ipc_buffer *hb = fault_handler(40, 110);
    struct cspace c40 = task_cs(40);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &c40, 2, 1) == T_OK);
    go(40);
    /* #BP is a trap: RIP is the next instruction. */
    CHECK(spawn(10, P(f_bp), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0));
    /* An IPC buffer with an offer: ignored for a fault (P69). */
    CHECK(thread_bind(&uts, 10 + T_THR, TS_IPCBUF, &uts, 10 + T_DATA) == T_OK);
    tbuf(10)->recv_count = 3;
    tbuf(10)->recv_slots[0] = 5;
    tbuf(10)->words[0] = 0xABAB;
    run_user();
    CHECK(hb->words[0] == 3 && hb->words[1] == 0 && hb->words[2] == 0);
    CHECK(hb->words[3] == UCODE_VA + 1 && th(10)->offer.count == 0);
    /* The binding has GRANT, but a fault reply carries nothing. */
    hb->caps[0] = 2;
    struct cspace c10 = task_cs(10);
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    const struct thread *who[] = { th(10), th(40) };
    snapshot(&snap_a, who, 2);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(0, 0) | CAPS(1)) == T_EPERM);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(0, 1)) == T_EINVAL);
    world_take(&world_b);
    snapshot(&snap_b, who, 2);
    CHECK(world_same(&world_a, &world_b) && memcmp(&snap_a, &snap_b, sizeof(snap_a)) == 0);
    CHECK(endpoint_reply(th(40), &c40, MSG_INFO(7, 0)) == T_OK);
    CHECK(tstate(10) == THREAD_FAULTED && tbuf(10)->words[0] == 0xABAB);
    CHECK(c10.slots[5].type == CAP_NULL);
    unpark(40);
    ut_teardown();

    /* A kernel-half address the thread itself used is reported as is. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    hb = fault_handler(40, 110);
    go(40);
    CHECK(spawn(10, P(f_pf_kernel), false, NULL));
    CHECK(fault_bind(10, 1, CAP_R_WRITE, 0));
    run_user();
    CHECK(hb->words[0] == 14 && hb->words[2] == 0xFFFF800000001000ull);
    CHECK(hb->words[1] == 5 && hb->words[3] == UCODE_VA + 10);   /* present, user */
    unpark(40);
    ut_teardown();

    /* No thread can forge the fault bit; the label alone is not proof. */
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    e = endpoint_of(&ut_store[1]);
    CHECK(ep_thread(40, false, 0, 0, CAP_R_READ));
    CHECK(ep_thread(10, true, 0, 0, CAP_R_WRITE));
    th(10)->regs.rsi = FAULT_INFO;
    test_report_count = 0;
    run_user();
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && reports_with(T_EINVAL) == 1);
    CHECK(ep_thread(20, true, 0, 0, CAP_R_WRITE));
    th(20)->regs.rsi = ((uint64_t)FAULT_LABEL << 32) | FAULT_WORDS;
    test_report_count = 0;
    run_user();
    bool plain = false;
    for (unsigned i = 0; i < test_report_count; i++)
        if (test_reports[i].c == 0 && test_reports[i].b == (((uint64_t)FAULT_LABEL << 32) | FAULT_WORDS))
            plain = true;                             /* the receiver: no bit 11 */
    CHECK(plain && ep_ok(e));
    ut_teardown();
}

static void test_cpu_hardening(void)
{
    extern uintptr_t __stack_chk_guard;

    CHECK(read_cr0() & CR0_WP);
    CHECK(rdmsr(MSR_EFER) & EFER_NXE);
    CHECK(!cpu_sec.smep || (read_cr4() & CR4_SMEP));
    CHECK(!cpu_sec.smap || (read_cr4() & CR4_SMAP));
    CHECK(!cpu_sec.umip || (read_cr4() & CR4_UMIP));
    /* CI runs QEMU with -cpu max, which exposes all three. */
    CHECK(cpu_sec.smep && cpu_sec.smap && cpu_sec.umip);

    CHECK(__stack_chk_guard != 0x595e9fbd94fda766ull);
    CHECK((__stack_chk_guard & 0xFF) == 0);
}

/* ---- The independent counter oracle (ADR 0010, SC17-SC19a, gate SC31.3) ----
 *
 * Recount every object's live records, rights and frame aliases from
 * scratch and compare with what the kernel stores. Independence is the
 * point: the oracle finds records by scanning their storage (the root
 * cspace, every static test store, every live thread's binding slots, and
 * every CNode reachable from those), never by following a CDT link; it
 * reads a frame's mapping from the hardware entry, not from map_state;
 * and it locates each object's stored metadata itself, without calling
 * obj_meta() or any counting code. So a bug shared by the CDT and the
 * counters cannot make the two agree. */

#define ORACLE_MAX  16384
#define ORACLE_CN   256
static const struct cap *or_rec[ORACLE_MAX];
static unsigned or_n;
static uint64_t or_cnode[ORACLE_CN];
static unsigned or_ncn;
static bool or_overflow;

static void or_scan(const struct cap *slots, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const struct cap *c = &slots[i];
        if (c->type == CAP_NULL)
            continue;
        if (or_n == ORACLE_MAX) {
            or_overflow = true;
            return;
        }
        or_rec[or_n++] = c;
    }
}

/* Records come from storage; a CNode record adds its object's slots once. */
static void or_collect(void)
{
    static struct cap *const stores[] = {
        caps_store, cdt_store_a, cdt_store_b, ut_store, cdtm_a, cdtm_b, big_store, b_store,
    };
    static const uint32_t sizes[] = {
        ARRAY_SIZE(caps_store), ARRAY_SIZE(cdt_store_a), ARRAY_SIZE(cdt_store_b),
        ARRAY_SIZE(ut_store), CDT_N, CDT_N, BIG_N, ARRAY_SIZE(b_store),
    };
    const struct cspace *root = untyped_root_cspace();

    or_n = or_ncn = 0;
    or_overflow = false;
    or_scan(root->slots, root->nslots);
    for (size_t i = 0; i < ARRAY_SIZE(stores); i++)
        or_scan(stores[i], sizes[i]);
    for (const struct thread *t = thread_registry_first(); t; t = t->reg_next)
        or_scan(t->bind, TS_COUNT);
    for (unsigned k = 0; k < or_n; k++) {
        const struct cap *c = or_rec[k];
        bool seen = false;
        if (c->type != CAP_CNODE)
            continue;
        for (unsigned j = 0; j < or_ncn && !seen; j++)
            seen = or_cnode[j] == c->object;
        if (seen)
            continue;
        if (or_ncn == ORACLE_CN) {
            or_overflow = true;
            return;
        }
        or_cnode[or_ncn++] = c->object;
        or_scan(phys_to_virt(cap_obj_paddr(c)), 1u << (cap_obj_bits(c) - CAP_SLOT_BITS));
    }
}

static int or_by_ptr(const struct cap *a, const struct cap *b)
{
    return a < b ? -1 : a > b;
}

static int or_by_object(const struct cap *a, const struct cap *b)
{
    if (a->type != b->type)
        return a->type < b->type ? -1 : 1;
    if (a->object != b->object)
        return a->object < b->object ? -1 : 1;
    return or_by_ptr(a, b);
}

/* Heapsort: no recursion, no extra memory. */
static void or_sift(unsigned i, unsigned n, int (*cmp)(const struct cap *, const struct cap *))
{
    for (;;) {
        unsigned big = i, l = 2 * i + 1, r = l + 1;
        if (l < n && cmp(or_rec[l], or_rec[big]) > 0)
            big = l;
        if (r < n && cmp(or_rec[r], or_rec[big]) > 0)
            big = r;
        if (big == i)
            return;
        const struct cap *t = or_rec[i];
        or_rec[i] = or_rec[big];
        or_rec[big] = t;
        i = big;
    }
}

static void or_sort(int (*cmp)(const struct cap *, const struct cap *))
{
    for (unsigned i = or_n / 2; i-- > 0;)
        or_sift(i, or_n, cmp);
    for (unsigned n = or_n; n > 1; n--) {
        const struct cap *t = or_rec[0];
        or_rec[0] = or_rec[n - 1];
        or_rec[n - 1] = t;
        or_sift(0, n - 1, cmp);
    }
}

static bool or_has(const struct cap *c)
{
    unsigned lo = 0, hi = or_n;

    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (or_rec[mid] == c)
            return true;
        if (or_rec[mid] < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/* Where the kernel should have stored c's metadata, worked out here from
 * ADR 0010 SC19a's table rather than by asking the kernel. */
static const struct obj_meta *or_stored(const struct cap *c)
{
    const struct obj_meta *m;
    uint64_t obj;

    switch (c->type) {
    case CAP_FRAME:
    case CAP_ADDRESS_SPACE:
    case CAP_PAGE_TABLE:
        return objmeta_frame_entry(pa_raw(cap_obj_paddr(c)) / PAGE_SIZE, &m) ? m : NULL;
    case CAP_ENDPOINT:
        return &endpoint_of(c)->meta;
    case CAP_NOTIFICATION:
        return &notification_of(c)->meta;
    case CAP_THREAD:
        return &thread_of(c)->meta;
    case CAP_CNODE:
        return &((const struct cap *)phys_to_virt(cap_obj_paddr(c)))[0].slot_meta;
    case CAP_TEST_TOKEN:
        for (unsigned i = 0; objmeta_token_entry(i, &obj, &m); i++)
            if (m->live && obj == c->object)
                return m;
        return NULL;
    default:
        return NULL;
    }
}

/* An object's carver and serial, read where SC19a and D3 put them: an
 * Untyped's in its record, everyone else's in its metadata. */
static const struct cap *or_carver(const struct cap *c)
{
    if (c->type == CAP_UNTYPED)
        return c->ut.carver;
    const struct obj_meta *m = or_stored(c);
    return m ? m->carver : NULL;
}

static uint64_t or_serial(const struct cap *c)
{
    if (c->type == CAP_UNTYPED)
        return c->ut.serial;
    const struct obj_meta *m = or_stored(c);
    return m ? m->serial : 0;
}

static bool or_contains(const struct cap *ut, const struct cap *c)
{
    uint64_t ub = pa_raw(cap_obj_paddr(ut)), cb = pa_raw(cap_obj_paddr(c));
    return cb >= ub && cb + (1ull << cap_obj_bits(c)) <= ub + (1ull << cap_obj_bits(ut));
}

static bool meta_mismatch(const char *what, const struct cap *c)
{
    kprintf("    meta_audit: %s (type %u object %lx)\n", what, c->type, c->object);
    return false;
}

/* One object's records are or_rec[lo, hi). */
static bool or_group(unsigned lo, unsigned hi, unsigned *frame_objs, unsigned *token_objs)
{
    const struct cap *c = or_rec[lo];
    uint32_t live = hi - lo, nr = 0, nw = 0, aw = 0, ax = 0;

    for (unsigned k = lo; k < hi; k++) {
        const struct cap *x = or_rec[k];
        nr += (x->rights & CAP_R_READ) != 0;
        nw += (x->rights & CAP_R_WRITE) != 0;
        if (x->type == CAP_FRAME) {
            /* The hardware entry, not the record's own bookkeeping. */
            uint64_t e = x->map_entry ? *x->map_entry : 0;
            if (e & PTE_P) {
                aw += (e & PTE_W) != 0;
                ax += (e & PTE_NX) == 0;
            }
            aw += x->slot_flags == (SLOT_BINDING | TS_IPCBUF);
        }
    }
    if (c->type == CAP_DEBUG_CONSOLE)
        return true;   /* no object, no metadata (SC19a) */
    if (or_serial(c) == 0 || or_serial(c) >= objmeta_test_next_serial())
        return meta_mismatch("serial not issued", c);
    /* E1: a frozen object has a marked root among its own records (or
     * the freeze came from the test hook); a marked root's object is
     * frozen. */
    bool marked = false;
    for (unsigned k = lo; k < hi; k++)
        if (or_rec[k]->rights & CAP_R_REVOKING) {
            if (marked)
                return meta_mismatch("two marked roots for one object (E1.4)", c);
            marked = true;
        }
    if (c->type == CAP_UNTYPED) {
        /* O2: one record, which carries the metadata. */
        if (live != 1)
            return meta_mismatch("Untyped with several records", c);
        if (c->ut.flags & ~OBJ_FROZEN)
            return meta_mismatch("Untyped flags", c);
        if (marked != !!(c->ut.flags & OBJ_FROZEN) && !objmeta_test_freezes)
            return meta_mismatch("Untyped freeze without its root, or a root without it", c);
        return true;   /* a root may have no carver */
    }
    const struct obj_meta *m = or_stored(c);
    if (!m)
        return meta_mismatch("no metadata", c);
    if (m->live != live)
        return meta_mismatch("live count", c);
    if (marked != !!(m->flags & OBJ_FROZEN) && !objmeta_test_freezes)
        return meta_mismatch("freeze without its root, or a root without it (E1)", c);
    uint32_t bits = c->type == CAP_CNODE ? cap_obj_bits(c) << OBJ_BITS_SHIFT : 0;
    /* R3: an object with a record is never dying. Only a live endpoint or
     * notification may be listed, and only with a doomed direction. */
    uint32_t doom = 0;
    if (c->type == CAP_ENDPOINT || c->type == CAP_NOTIFICATION)
        doom = m->flags & (OBJ_LISTED | OBJ_DOOM_SEND | OBJ_DOOM_RECV);
    if ((m->flags & ~(OBJ_FROZEN | OBJ_BITS_MASK)) != doom || (m->flags & OBJ_BITS_MASK) != bits)
        return meta_mismatch("flags", c);
    if (doom && !(doom & OBJ_LISTED) != !(doom & (OBJ_DOOM_SEND | OBJ_DOOM_RECV)))
        return meta_mismatch("listed without a doomed direction", c);
    if (c->type == CAP_ENDPOINT || c->type == CAP_NOTIFICATION) {
        if (m->n_read != nr || m->n_write != nw)
            return meta_mismatch("rights counts", c);
    } else if (c->type == CAP_FRAME) {
        if (m->alias_w != aw || m->alias_x != ax)
            return meta_mismatch("alias counts", c);
    } else if (c->type == CAP_ADDRESS_SPACE || c->type == CAP_PAGE_TABLE) {
        /* D1: the filled count equals the present user entries in the
         * hardware table itself. */
        const uint64_t *t = phys_to_virt(cap_obj_paddr(c));
        uint32_t filled = 0;
        for (unsigned i = 0; i < (c->type == CAP_ADDRESS_SPACE ? 256u : 512u); i++)
            filled += (t[i] & PTE_P) != 0;
        if (m->filled != filled || m->filled_reserved)
            return meta_mismatch("filled-entry count", c);
    } else if (m->n_read || m->n_write) {
        return meta_mismatch("stray counts", c);   /* incl. a live CNode listed */
    }
    if (c->type == CAP_TEST_TOKEN) {
        ++*token_objs;
        return m->carver == NULL || meta_mismatch("token carver", c);
    }
    if (c->type == CAP_FRAME || c->type == CAP_ADDRESS_SPACE || c->type == CAP_PAGE_TABLE)
        ++*frame_objs;
    /* The carving back-pointer: a live Untyped record whose range holds
     * the object. Only an Untyped may have none (a boot or test root). */
    if (!m->carver)
        return c->type == CAP_UNTYPED || meta_mismatch("no carver", c);
    return true;
}

static uint64_t or_serials[ORACLE_MAX];
static unsigned or_nserial;

static void or_sort_serials(void)
{
    /* Insertion-free heapsort over or_serials[0, or_nserial). */
    unsigned n = or_nserial;
    for (unsigned start = n / 2; start-- > 0;) {
        for (unsigned i = start;;) {
            unsigned big = i, l = 2 * i + 1, r = l + 1;
            if (l < n && or_serials[l] > or_serials[big]) big = l;
            if (r < n && or_serials[r] > or_serials[big]) big = r;
            if (big == i) break;
            uint64_t t = or_serials[i]; or_serials[i] = or_serials[big]; or_serials[big] = t;
            i = big;
        }
    }
    for (unsigned end = n; end > 1; end--) {
        uint64_t t = or_serials[0]; or_serials[0] = or_serials[end - 1]; or_serials[end - 1] = t;
        for (unsigned i = 0, m = end - 1;;) {
            unsigned big = i, l = 2 * i + 1, r = l + 1;
            if (l < m && or_serials[l] > or_serials[big]) big = l;
            if (r < m && or_serials[r] > or_serials[big]) big = r;
            if (big == i) break;
            uint64_t u = or_serials[i]; or_serials[i] = or_serials[big]; or_serials[big] = u;
            i = big;
        }
    }
}

static bool meta_audit(void)
{
    static const struct obj_meta zero;
    const struct obj_meta *m;
    uint64_t obj;
    unsigned frame_objs = 0, token_objs = 0, threads = 0, thread_objs = 0;
    bool ok = true;

    or_nserial = 0;
    or_collect();
    if (or_overflow) {
        kprintf("    meta_audit: too many records\n");
        return false;
    }

    /* By address: every record is counted once, and carvers are found. */
    or_sort(or_by_ptr);
    for (unsigned k = 1; k < or_n; k++)
        if (or_rec[k] == or_rec[k - 1])
            return meta_mismatch("record found twice", or_rec[k]);
    for (unsigned k = 0; k < or_n && ok; k++) {
        const struct cap *c = or_rec[k];
        const struct cap *cv = c->type == CAP_DEBUG_CONSOLE ? NULL : or_carver(c);
        if (cv && !(or_has(cv) && cv->type == CAP_UNTYPED && or_contains(cv, c)))
            ok = meta_mismatch("carver is not a live Untyped holding the object", c);
        /* SC21: an object is created after the Untyped it is carved from. */
        if (ok && cv && or_serial(cv) >= or_serial(c))
            ok = meta_mismatch("serial not after its carver's", c);
    }

    /* By object: each group is one object's records. */
    or_sort(or_by_object);
    for (unsigned lo = 0, hi; ok && lo < or_n; lo = hi) {
        for (hi = lo + 1; hi < or_n && or_rec[hi]->type == or_rec[lo]->type &&
                          or_rec[hi]->object == or_rec[lo]->object; hi++)
            ;
        ok = or_group(lo, hi, &frame_objs, &token_objs);
        thread_objs += or_rec[lo]->type == CAP_THREAD;
        if (or_rec[lo]->type != CAP_DEBUG_CONSOLE) {
            KASSERT(or_nserial < ORACLE_MAX);
            or_serials[or_nserial++] = or_serial(or_rec[lo]);
        }
    }
    if (!ok)
        return false;
    /* D3: no two objects share a serial. */
    or_sort_serials();
    for (unsigned k = 1; k < or_nserial; k++)
        if (or_serials[k] == or_serials[k - 1]) {
            kprintf("    meta_audit: serial %lu used twice\n", or_serials[k]);
            return false;
        }

    /* E1: every marked root belongs to exactly one revocation
     * continuation, found from the thread's side by slot and identity;
     * and every revocation continuation finds its marked root. */
    unsigned marked = 0, owners = 0;
    for (unsigned k = 0; k < or_n; k++)
        marked += (or_rec[k]->rights & CAP_R_REVOKING) != 0;
    for (const struct thread *t = thread_registry_first(); t; t = t->reg_next) {
        struct cspace tcs;
        if (t->cont.kind != CONT_REVOKE)
            continue;
        owners++;
        if (cnode_view_cap(&t->bind[TS_CSPACE], &tcs) != T_OK || t->cont.slot >= tcs.nslots ||
            !(tcs.slots[t->cont.slot].rights & CAP_R_REVOKING) ||
            !obj_identity_is(&tcs.slots[t->cont.slot], t->cont.type, t->cont.object,
                             t->cont.serial)) {
            kprintf("    meta_audit: a revocation continuation without its marked root\n");
            return false;
        }
    }
    if (marked != owners) {
        kprintf("    meta_audit: %u marked roots, %u revocation continuations\n", marked, owners);
        return false;
    }

    /* D8: the reaper's list, against the Untypeds found by the scan.
     * Every entry is listed once and flagged as listed; a dying entry has
     * no records, carries a serial, and was carved from a live Untyped;
     * and each Untyped's dying count equals the dying entries naming it
     * (counted here from the list, never read from the kernel's tally). */
    static const struct obj_meta *listed[ORACLE_CN * 4];
    unsigned nl = 0;
    or_sort(or_by_ptr);   /* or_has() searches by address */
    uint64_t cur = 0;
    unsigned kind;
    const struct obj_meta *lm;
    while (reaper_list_next(&cur, &kind, &lm)) {
        if (nl == ARRAY_SIZE(listed)) {
            kprintf("    meta_audit: reaper list too long or cyclic\n");
            return false;
        }
        for (unsigned j = 0; j < nl; j++)
            if (listed[j] == lm) {
                kprintf("    meta_audit: listed twice (R2)\n");
                return false;
            }
        listed[nl++] = lm;
        bool dying = lm->flags & OBJ_DYING;
        if (!(lm->flags & OBJ_LISTED) || (dying && (lm->live || !lm->serial || !lm->carver)) ||
            (!dying && !(lm->flags & (OBJ_DOOM_SEND | OBJ_DOOM_RECV))) ||
            (dying && !(or_has(lm->carver) && lm->carver->type == CAP_UNTYPED))) {
            kprintf("    meta_audit: bad reaper entry (kind %u flags %x)\n", kind, lm->flags);
            return false;
        }
    }
    for (unsigned k = 0; k < or_n; k++) {
        const struct cap *u = or_rec[k];
        uint32_t want = 0;
        if (u->type != CAP_UNTYPED)
            continue;
        for (unsigned j = 0; j < nl; j++)
            want += (listed[j]->flags & OBJ_DYING) && listed[j]->carver == u;
        if (u->ut.dying != want) {
            kprintf("    meta_audit: Untyped dying count %u, %u listed\n", u->ut.dying, want);
            return false;
        }
    }

    /* Nothing is stored for an object with no records: every non-zero
     * frame table entry and token entry belongs to an object found above,
     * and a thread object is registered exactly while it has records. */
    unsigned nonzero = 0, tokens = 0;
    for (uint64_t i = 0; objmeta_frame_entry(i, &m); i++)
        nonzero += memcmp(m, &zero, sizeof(zero)) != 0;
    for (unsigned i = 0; objmeta_token_entry(i, &obj, &m); i++)
        tokens += m->live != 0;
    for (const struct thread *t = thread_registry_first(); t; t = t->reg_next)
        threads++;
    if (nonzero != frame_objs) {
        kprintf("    meta_audit: %u frame table entries in use, %u objects\n", nonzero, frame_objs);
        return false;
    }
    if (tokens != token_objs || threads != thread_objs) {
        kprintf("    meta_audit: tokens %u/%u threads %u/%u\n", tokens, token_objs, threads,
                thread_objs);
        return false;
    }
    return true;
}

/* ---- Stage 2: counters, metadata and the freeze (ADR 0010, SC17-SC22a) ---- */

static const struct obj_meta meta_zero;

static bool meta_is(const struct cap *c, uint32_t live, uint32_t a, uint32_t b)
{
    const struct obj_meta *m = or_stored(c);
    return m && m->live == live && m->n_read == a && m->n_write == b;
}

/* SC17, SC19a, C4-C6: counts move with records, only in commit steps;
 * finality is the count reaching 0, whichever record goes last. */
static void test_meta_counts(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    const struct obj_meta *m = or_stored(&ut_store[1]);
    CHECK(meta_is(&ut_store[1], 1, 1, 1) && m->carver == &ut_store[0] && m->flags == 0);
    CHECK(cap_derive(&uts, 1, 2, CAP_R_READ | G_, 0) == T_OK);
    CHECK(cap_derive(&uts, 2, 3, CAP_R_READ, 0) == T_OK);
    CHECK(cap_derive(&uts, 1, 4, CAP_R_WRITE, 0x9) == T_OK);
    CHECK(cap_derive(&uts, 1, 5, 0, 0) == T_OK);           /* no rights: live only */
    CHECK(meta_is(&ut_store[1], 5, 3, 2) && meta_audit());

    /* C5: every refusal leaves every count as it was. */
    struct obj_meta before = *m;
    CHECK(cap_derive(&uts, 3, 6, CAP_R_ALL, 0) == T_EPERM);   /* no GRANT */
    CHECK(cap_derive(&uts, 1, 2, CAP_R_ALL, 0) == T_EBUSY);   /* occupied */
    CHECK(cap_derive(&uts, 1, 999, CAP_R_ALL, 0) == T_ERANGE);
    CHECK(cap_derive(&uts, 1, 6, CAP_R_ALL, 0) == T_OK && cap_delete(&uts, 6) == T_OK);
    CHECK(memcmp(&before, m, sizeof(before)) == 0 && meta_audit());

    /* C6: deleting the first record re-parents its children and changes
     * no other count; it is not final, whatever its position. */
    CHECK(cap_delete(&uts, 1) == T_OK);
    CHECK(meta_is(&ut_store[2], 4, 2, 1) && ut_store[2].type == CAP_ENDPOINT && meta_audit());
    CHECK(cap_delete(&uts, 3) == T_OK && cap_delete(&uts, 2) == T_OK);
    CHECK(meta_is(&ut_store[4], 2, 0, 1) && meta_audit());
    CHECK(cap_delete(&uts, 4) == T_OK && meta_is(&ut_store[5], 1, 0, 0));
    /* The last record: the count reaches 0 and the metadata is cleared,
     * carver included, so nothing stale survives into the memory's next
     * object. */
    CHECK(cap_delete(&uts, 5) == T_OK && memcmp(m, &meta_zero, sizeof(meta_zero)) == 0);
    CHECK(meta_audit());

    /* Every kind of location: retype gives each new object a count of 1
     * and its carver. */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 14, &uts, 10, 1) == T_OK);
    CHECK(cap_retype(&uts, 10, CAP_NOTIFICATION, 0, &uts, 11, 2) == T_OK);
    CHECK(cap_retype(&uts, 10, CAP_CNODE, 2, &uts, 13, 1) == T_OK);
    CHECK(cap_retype(&uts, 10, CAP_FRAME, 0, &uts, 14, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_THREAD, 0, &uts, 15, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_ADDRESS_SPACE, 0, &uts, 16, 1) == T_OK);
    CHECK(ut_store[10].ut.carver == &ut_store[0]);
    CHECK(ut_store[10].ut.serial > ut_store[0].ut.serial);
    for (cap_slot_t k = 11; k <= 14; k++)
        CHECK(or_stored(&ut_store[k])->live == 1 && or_stored(&ut_store[k])->carver == &ut_store[10]);
    CHECK(or_stored(&ut_store[11]) != or_stored(&ut_store[12]));   /* per object */
    CHECK(or_stored(&ut_store[15])->carver == &ut_store[0] &&
          or_stored(&ut_store[16])->carver == &ut_store[0]);
    /* A CNode's metadata is a property of its slot 0: a capability stored
     * there and deleted leaves it alone. */
    struct cspace cn;
    CHECK(cnode_view(&uts, 13, &cn) == T_OK);
    CHECK(cap_derive_to(&uts, 11, &cn, 0, CAP_R_ALL, 0) == T_OK && meta_audit());
    CHECK(cnode_view(&uts, 13, &cn) == T_OK && cap_delete(&cn, 0) == T_OK);
    CHECK(or_stored(&ut_store[13])->live == 1 && or_stored(&ut_store[13])->carver == &ut_store[10]);
    CHECK(cap_derive(&uts, 13, 17, CAP_R_ALL, 0) == T_OK && or_stored(&ut_store[13])->live == 2);
    CHECK(meta_audit());
    ut_teardown();
    CHECK(meta_audit());
}

/* SC18: frame alias counts across map, protect, unmap, binding and
 * deletion, checked against the hardware entries by the oracle. */
static void test_meta_frames(void)
{
    CHECK(ut_setup(20));
    CHECK(build_as(1, 2));
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 10, 1) == T_OK);
    CHECK(cap_derive(&uts, 10, 11, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive(&uts, 10, 12, CAP_R_ALL, 0) == T_OK);
    const struct obj_meta *m = or_stored(&ut_store[10]);
    CHECK(m->live == 3 && m->alias_w == 0 && m->alias_x == 0);
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), RW) == T_OK && m->alias_w == 1 && meta_audit());
    CHECK(vspace_map(&uts, 1, 11, ua(VA0 + 0x1000), RW) == T_OK && m->alias_w == 2);
    /* Protecting to the same rights keeps the alias: still writable. */
    CHECK(vspace_protect(&uts, 11, RW) == T_OK && m->alias_w == 2 && meta_audit());
    CHECK(vspace_protect(&uts, 10, RO) == T_OK && m->alias_w == 1 && meta_audit());
    CHECK(vspace_map(&uts, 1, 12, ua(VA0 + 0x2000), RX) == T_EPERM);   /* 11 is RW */
    CHECK(vspace_protect(&uts, 11, RO) == T_OK && m->alias_w == 0);
    CHECK(vspace_map(&uts, 1, 12, ua(VA0 + 0x2000), RX) == T_OK && m->alias_x == 1);
    CHECK(vspace_protect(&uts, 12, RX) == T_OK && m->alias_x == 1 && meta_audit());
    CHECK(vspace_protect(&uts, 12, RO) == T_OK && m->alias_x == 0 && meta_audit());
    CHECK(vspace_protect(&uts, 12, RX) == T_EPERM && m->alias_x == 0);  /* V7 */
    /* Failed maps change nothing. */
    struct obj_meta before = *m;
    CHECK(vspace_map(&uts, 1, 12, ua(VA0 + 0x3000), RW) == T_EBUSY);   /* already mapped */
    CHECK(vspace_map(&uts, 1, 10, ua(VA0 + 0x3000), RO) == T_EBUSY);
    CHECK(vspace_unmap(&uts, 12) == T_OK);
    before = *m;
    CHECK(vspace_map(&uts, 1, 12, ua(VA0), RX) == T_EBUSY);              /* VA taken */
    CHECK(vspace_map(&uts, 1, 12, ua(VA0 + (1ull << 30)), RX) == T_ENOENT); /* no table */
    CHECK(memcmp(&before, m, sizeof(before)) == 0 && meta_audit());
    CHECK(vspace_map(&uts, 1, 12, ua(VA0 + 0x2000), RW) == T_OK);
    CHECK(m->alias_w == 1 && meta_audit());
    /* Deleting a mapped record removes its alias in the same deletion. */
    CHECK(cap_delete(&uts, 12) == T_OK && m->alias_w == 0 && m->live == 2 && meta_audit());
    /* Tearing down the page tables detaches the mappings under them. */
    CHECK(cap_delete(&uts, 4) == T_OK && m->alias_w == 0 && m->alias_x == 0 && meta_audit());
    CHECK(ut_store[10].map_entry == NULL && ut_store[11].map_entry == NULL);
    /* The last record clears the frame table entry. */
    CHECK(cap_delete(&uts, 11) == T_OK && cap_delete(&uts, 10) == T_OK);
    CHECK(memcmp(m, &meta_zero, sizeof(meta_zero)) == 0 && meta_audit());
    ut_teardown();
}

/* SC18: a count with no room refuses a new record with T_ERANGE and
 * changes nothing; derivation, binding and transfer all meet it. The
 * count is raised by hand (2^32 records would need 512 GiB) and restored
 * before the oracle looks. */
static void test_meta_overflow(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &uts, 2, 1) == T_OK);
    CHECK(xfer_receiver(40, (const uint32_t[]){ 7 }, 1));
    run_user();
    CHECK(tstate(40) == THREAD_BLOCKED_RECV);
    CHECK(xfer_sender(10, 6, 0xCD, (const uint32_t[]){ 5 }, 1));
    struct cspace c10 = task_cs(10);
    CHECK(cap_derive_to(&uts, 2, &c10, 5, CAP_R_ALL, 0) == T_OK);
    CHECK(meta_audit());

    struct obj_meta *m = (struct obj_meta *)or_stored(&ut_store[2]);
    struct obj_meta *me = (struct obj_meta *)or_stored(&ut_store[1]);
    struct obj_meta real = *m, real_e = *me;
    m->live = OBJ_LIVE_MAX + 1;   /* as if 2^32 - 3 records existed */
    me->live = OBJ_LIVE_MAX + 1;
    struct obj_meta before = *m;
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    CHECK(cap_derive(&uts, 2, 3, CAP_R_ALL, 0) == T_ERANGE);
    CHECK(thread_bind(&uts, 10 + T_THR, TS_FAULT, &uts, 1) == T_ERANGE);
    run_user();   /* the transfer: refused at capture, receiver untouched */
    CHECK(test_report_count == 1 && test_reports[0].a == (uint64_t)T_ERANGE);
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b) && memcmp(&before, m, sizeof(before)) == 0);
    CHECK(tstate(40) == THREAD_BLOCKED_RECV);

    m->live = OBJ_LIVE_MAX;       /* exactly at the limit: still allowed */
    CHECK(cap_derive(&uts, 2, 3, CAP_R_ALL, 0) == T_OK && m->live == OBJ_LIVE_MAX + 1);
    CHECK(cap_delete(&uts, 3) == T_OK && m->live == OBJ_LIVE_MAX);
    *m = real;
    *me = real_e;
    CHECK(meta_audit());
    ut_teardown();
}

/* SC22a: a frozen object, or an object under a frozen Untyped, gets no
 * new record by derive, binding, transfer or retype; T_EBUSY, nothing
 * changed; other objects unaffected. */
static void test_meta_freeze(void)
{
    CHECK(ut_setup(20));
    /* U0 (slot 0) > U1 (20) > U2 (21); objects under each. */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &uts, 20, 1) == T_OK);
    CHECK(cap_retype(&uts, 20, CAP_UNTYPED, 14, &uts, 21, 1) == T_OK);
    CHECK(cap_retype(&uts, 21, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);   /* under U2 */
    CHECK(cap_retype(&uts, 21, CAP_FRAME, 0, &uts, 2, 1) == T_OK);      /* under U2 */
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 3, 1) == T_OK);    /* under U0 only */
    /* An unrelated Untyped straight from a root: never affected. */
    struct cspace *root = untyped_root_cspace();
    cap_slot_t r = 0;
    while (r < root->nslots && !(root->slots[r].type == CAP_UNTYPED && cap_obj_bits(&root->slots[r]) > 16))
        r++;
    CHECK(r < root->nslots && cap_retype(root, r, CAP_UNTYPED, 16, &uts, 30, 1) == T_OK);
    CHECK(cap_retype(&uts, 30, CAP_ENDPOINT, 0, &uts, 31, 1) == T_OK);
    CHECK(spawn(40, P(hello), false, NULL));
    CHECK(thread_suspend(&uts, 40 + T_THR) == T_OK);
    CHECK(meta_audit());

    world_tables((const cap_slot_t[]){ 40 }, 1);
    /* Freezing the object itself. */
    objmeta_test_freeze(&ut_store[1], true);
    world_take(&world_a);
    CHECK(cap_derive(&uts, 1, 4, CAP_R_ALL, 0) == T_EBUSY);
    CHECK(thread_bind(&uts, 40 + T_THR, TS_FAULT, &uts, 1) == T_EBUSY);
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b));
    /* Only that object: a sibling under the same Untyped, and the
     * Untyped itself, are unaffected. */
    CHECK(cap_derive(&uts, 2, 4, CAP_R_ALL, 0) == T_OK && cap_delete(&uts, 4) == T_OK);
    CHECK(cap_retype(&uts, 21, CAP_ENDPOINT, 0, &uts, 4, 1) == T_OK && cap_delete(&uts, 4) == T_OK);
    objmeta_test_freeze(&ut_store[1], false);
    CHECK(cap_derive(&uts, 1, 4, CAP_R_ALL, 0) == T_OK && cap_delete(&uts, 4) == T_OK);

    /* Freezing U0: everything carved under it, at any depth, is blocked;
     * the chain is followed U2 -> U1 -> U0. */
    objmeta_test_freeze(&ut_store[0], true);
    world_take(&world_a);
    CHECK(cap_derive(&uts, 1, 4, CAP_R_ALL, 0) == T_EBUSY);   /* two links up */
    CHECK(cap_derive(&uts, 2, 4, CAP_R_ALL, 0) == T_EBUSY);
    CHECK(cap_derive(&uts, 3, 4, CAP_R_ALL, 0) == T_EBUSY);   /* one link */
    CHECK(thread_bind(&uts, 40 + T_THR, TS_FAULT, &uts, 3) == T_EBUSY);
    CHECK(thread_bind(&uts, 40 + T_THR, TS_IPCBUF, &uts, 2) == T_EBUSY);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 4, 1) == T_EBUSY);
    CHECK(cap_retype(&uts, 20, CAP_FRAME, 0, &uts, 4, 1) == T_EBUSY);
    CHECK(cap_retype(&uts, 21, CAP_FRAME, 0, &uts, 4, 1) == T_EBUSY);
    /* The task's own objects are under U0 too. */
    struct cspace c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 40 + T_THR, &c40, 2, CAP_R_ALL, 0) == T_EBUSY);
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b));
    /* Unrelated objects are untouched by the freeze. */
    CHECK(cap_derive(&uts, 31, 4, CAP_R_ALL, 0) == T_OK && cap_delete(&uts, 4) == T_OK);
    CHECK(cap_retype(&uts, 30, CAP_FRAME, 0, &uts, 4, 1) == T_OK && cap_delete(&uts, 4) == T_OK);
    /* Deletion is not creation: still allowed under a freeze (F1). */
    CHECK(cap_delete(&uts, 3) == T_OK);
    objmeta_test_freeze(&ut_store[0], false);

    /* Freezing the middle of the chain: only what lies under U1. */
    objmeta_test_freeze(&ut_store[20], true);
    CHECK(cap_derive(&uts, 1, 4, CAP_R_ALL, 0) == T_EBUSY);
    CHECK(cap_retype(&uts, 21, CAP_FRAME, 0, &uts, 4, 1) == T_EBUSY);
    CHECK(cap_retype(&uts, 20, CAP_FRAME, 0, &uts, 4, 1) == T_EBUSY);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 4, 1) == T_OK && cap_delete(&uts, 4) == T_OK);
    objmeta_test_freeze(&ut_store[20], false);
    CHECK(cap_derive(&uts, 1, 4, CAP_R_ALL, 0) == T_OK);
    CHECK(meta_audit());

    /* Transfer: a sender offering a capability under a frozen Untyped is
     * refused before anything moves; the receiver stays as it was. */
    ut_teardown();
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 14, &uts, 20, 1) == T_OK);
    CHECK(cap_retype(&uts, 20, CAP_NOTIFICATION, 0, &uts, 21, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(xfer_receiver(40, (const uint32_t[]){ 7 }, 1));
    run_user();
    CHECK(tstate(40) == THREAD_BLOCKED_RECV);
    CHECK(xfer_sender(10, 6, 0xCD, (const uint32_t[]){ 5 }, 1));
    struct cspace c10 = task_cs(10);
    CHECK(cap_derive_to(&uts, 21, &c10, 5, CAP_R_ALL, 0) == T_OK);
    objmeta_test_freeze(&ut_store[20], true);
    world_tables((const cap_slot_t[]){ 10, 40 }, 2);
    world_take(&world_a);
    test_report_count = 0;
    run_user();
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b));
    CHECK(test_report_count == 1 && test_reports[0].a == (uint64_t)T_EBUSY);
    CHECK(tstate(40) == THREAD_BLOCKED_RECV && ep_ok(e));
    objmeta_test_freeze(&ut_store[20], false);
    ut_teardown();
    CHECK(meta_audit());
}

/* ---- Stage 3: identity, dying objects and the reaper (ADR 0010 SC21,
 * SC23, SC24, R0-R9; revision 3d D0-D7) ---- */

struct ident { uint32_t type; uint64_t object, serial; };

static struct ident ident_of(const struct cap *c)
{
    return (struct ident){ c->type, c->object, obj_serial(c) };
}

static bool ident_is(const struct cap *c, struct ident id)
{
    return obj_identity_is(c, id.type, id.object, id.serial);
}

/* R0 and R4: an endpoint with more waiters than one reaper chunk dies.
 * Its slot is free at once; it is dying, pinned and listed; its memory is
 * not reusable and its Untyped not deletable until the reaper's last
 * unit; each waiter is failed exactly once, at most REAP_CHUNK per
 * boundary. */
static void test_reaper_dying(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &uts, 100, 1) == T_OK);   /* Ua */
    CHECK(cap_retype(&uts, 100, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    const cap_slot_t w[] = { 10, 20, 30, 40, 50, 60 };   /* > REAP_CHUNK */
    for (unsigned i = 0; i < ARRAY_SIZE(w); i++)
        CHECK(ep_thread(w[i], false, 0, 0, CAP_R_READ));
    run_user();
    for (unsigned i = 0; i < ARRAY_SIZE(w); i++) {
        struct cspace c = task_cs(w[i]);
        CHECK(tstate(w[i]) == THREAD_BLOCKED_RECV && cap_delete(&c, 1) == T_OK);
    }
    uint64_t serial = e->meta.serial;
    CHECK(reaper_idle() && ut_store[100].ut.dying == 0);

    /* The death transaction (R0): after it, no capability, dying, pinned,
     * listed, the slot empty and reusable; nothing else changed. */
    CHECK(cap_delete(&uts, 1) == T_OK);
    CHECK(ut_store[1].type == CAP_NULL && !cap_has_children(&ut_store[100]));
    /* (Its last record was also the last WRITE, so the receivers were
     * doomed first, in the same deletion.) */
    CHECK(e->meta.live == 0 && (e->meta.flags & (OBJ_DYING | OBJ_LISTED)) == (OBJ_DYING | OBJ_LISTED));
    CHECK(e->meta.serial == serial && e->meta.carver == &ut_store[100]);
    CHECK(ut_store[100].ut.dying == 1 && !reaper_idle());
    for (unsigned i = 0; i < ARRAY_SIZE(w); i++)
        CHECK(tstate(w[i]) == THREAD_BLOCKED_RECV);
    CHECK(ep_ok(e) && AUDIT());

    /* Amended O7/O8 while it is dying: a retype that needs a reset gets
     * T_EAGAIN with nothing changed; the Untyped cannot be deleted. */
    world_tables(w, 2);
    world_take(&world_a);
    CHECK(cap_retype(&uts, 100, CAP_FRAME, 0, &uts, 101, 16) == T_EAGAIN);
    CHECK(cap_delete(&uts, 100) == T_EBUSY);
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b));

    /* R5: at most REAP_CHUNK units per boundary, one waiter per unit,
     * in queue order, each exactly once. */
    CHECK(reaper_run(REAP_CHUNK) == REAP_CHUNK);
    for (unsigned i = 0; i < ARRAY_SIZE(w); i++)
        CHECK(tstate(w[i]) == (i < REAP_CHUNK ? THREAD_RUNNABLE : THREAD_BLOCKED_RECV));
    CHECK(ut_store[100].ut.dying == 1 && AUDIT());
    reap_all();
    CHECK(reaper_idle() && ut_store[100].ut.dying == 0 && e->meta.flags == 0);
    CHECK(e->meta.serial == 0 && e->meta.carver == NULL);   /* retired: nothing stale */
    run_user();
    CHECK(test_report_count == ARRAY_SIZE(w) && reports_with(T_EDEAD) == ARRAY_SIZE(w));

    /* Only now is the memory reusable, and the Untyped deletable. */
    CHECK(cap_retype(&uts, 100, CAP_FRAME, 0, &uts, 101, 16) == T_OK);
    CHECK(slot_paddr(101) == slot_paddr(100));   /* reset: from the start again */
    for (cap_slot_t k = 101; k < 117; k++)
        CHECK(cap_delete(&uts, k) == T_OK);
    CHECK(cap_delete(&uts, 100) == T_OK && AUDIT());
    ut_teardown();
}

/* D7, the ABA lifecycle, in two phases. While A is dying, a new object
 * can take A's slot but never A's address (the pin, R4); after A's
 * teardown, C takes A's address and slot with a newer serial. A stale
 * identity of A never matches, and A's reaper never touches B. */
static void test_reaper_aba(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &uts, 100, 1) == T_OK);   /* Ua */
    CHECK(cap_retype(&uts, 100, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);   /* A in X = 1 */
    /* B from a different Untyped: once A dies, Ua holds nothing alive, so
     * only A's pin (its dying count) stands between Ua and a reset that
     * would put the next object at A's address. */
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 5, 1) == T_OK);     /* B in 5 */
    struct endpoint *a = endpoint_of(&ut_store[1]), *b = endpoint_of(&ut_store[5]);
    struct ident ida = ident_of(&ut_store[1]), idb = ident_of(&ut_store[5]);
    const cap_slot_t w[] = { 10, 20, 30, 40, 50, 60 };
    for (unsigned i = 0; i < ARRAY_SIZE(w); i++)
        CHECK(ep_thread(w[i], false, 0, 0, CAP_R_READ));
    CHECK(ep_thread_on(70, 5, false, 0, 0, CAP_R_READ));                /* B's waiter */
    run_user();
    CHECK(a->receivers.head == th(w[0]) && b->receivers.head == th(70));
    for (unsigned i = 0; i < ARRAY_SIZE(w); i++) {
        struct cspace c = task_cs(w[i]);
        CHECK(cap_delete(&c, 1) == T_OK);
    }
    CHECK(cap_delete(&uts, 1) == T_OK);                       /* A dies */
    CHECK(a->meta.flags & OBJ_DYING);

    /* Phase 1. A new object retyped into A's slot, from A's Untyped, while
     * A is pending. Ua has no children, so a reset would be allowed if A
     * were not pinned: D must still land above A, never at A's address. */
    CHECK(!cap_has_children(&ut_store[100]) && ut_store[100].ut.dying == 1);
    CHECK(cap_retype(&uts, 100, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);  /* D in X */
    struct endpoint *d = endpoint_of(&ut_store[1]);
    struct ident idd = ident_of(&ut_store[1]);
    CHECK(idd.object != ida.object && idd.object != idb.object && idd.serial > idb.serial);
    CHECK(!ident_is(&ut_store[1], ida) && ident_is(&ut_store[1], idd));
    CHECK(cap_derive(&uts, 1, 6, CAP_R_ALL, 0) == T_OK);      /* keep D, move it aside */
    CHECK(cap_delete(&uts, 1) == T_OK && d->meta.live == 1);
    CHECK(cap_derive(&uts, 5, 1, CAP_R_ALL, 0) == T_OK);      /* B into X */
    CHECK(!ident_is(&ut_store[1], ida) && ident_is(&ut_store[1], idb));
    struct obj_meta bm = b->meta, dm = d->meta;

    /* Interleave A's reaper work with operations on B. */
    CHECK(reaper_run(1) == 1 && tstate(w[0]) == THREAD_RUNNABLE);
    CHECK(cap_derive(&uts, 1, 2, CAP_R_ALL, 0) == T_OK && b->meta.live == bm.live + 1);
    CHECK(reaper_run(2) == 2 && AUDIT());
    CHECK(cap_delete(&uts, 2) == T_OK);
    CHECK(reaper_run(1) == 1 && ep_ok(b) && ep_ok(a));
    for (cap_slot_t k = 0; k < ARRAY_SIZE(ut_store); k++)
        CHECK(!ident_is(&ut_store[k], ida));                  /* nothing names A */
    CHECK(memcmp(&b->meta, &bm, sizeof(bm)) == 0 && b->receivers.head == th(70) &&
          tstate(70) == THREAD_BLOCKED_RECV);                 /* B untouched */
    CHECK(memcmp(&d->meta, &dm, sizeof(dm)) == 0 && ep_ok(d) &&
          !d->senders.head && !d->receivers.head);            /* D untouched */
    CHECK(a->meta.flags & OBJ_DYING);                         /* A still pending */
    /* (B is live in Ua, so no reset could happen anyway; reaper_dying
     * covers the T_EAGAIN of a reset blocked only by a dying object.) */
    reap_all();
    CHECK(ut_store[100].ut.dying == 0 && tstate(70) == THREAD_BLOCKED_RECV);  /* B's waiter stays */
    CHECK(memcmp(&b->meta, &bm, sizeof(bm)) == 0 && ep_ok(b));
    CHECK(memcmp(&d->meta, &dm, sizeof(dm)) == 0 && ep_ok(d));
    CHECK(cap_delete(&uts, 6) == T_OK);                       /* D goes */

    /* B goes too (its waiter is failed by the reaper), and every waiter
     * of either was failed exactly once. */
    struct cspace c70 = task_cs(70);
    CHECK(cap_delete(&c70, 1) == T_OK && cap_delete(&uts, 1) == T_OK && cap_delete(&uts, 5) == T_OK);
    reap_all();
    run_user();
    CHECK(reports_with(T_EDEAD) == ARRAY_SIZE(w) + 1 && test_report_count == ARRAY_SIZE(w) + 1);

    /* Phase 2. After the teardowns the memory resets, and C lands at A's
     * address and in A's slot with a newer serial. Neither A's identity
     * nor B's matches it. */
    CHECK(!cap_has_children(&ut_store[100]) && ut_store[100].ut.dying == 0);
    CHECK(cap_retype(&uts, 100, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct ident idc = ident_of(&ut_store[1]);
    CHECK(idc.object == ida.object && idc.serial > idd.serial);   /* A's address, reset */
    CHECK(!ident_is(&ut_store[1], ida) && !ident_is(&ut_store[1], idb) &&
          ident_is(&ut_store[1], idc));
    CHECK(AUDIT());
    ut_teardown();
}

/* D2's proof at run time. X, a dying CNode, holds Untyped U in slot 0;
 * Y1..Y3 were carved from U and become dying while X is emptied. The
 * reaper must requeue X (never wait, never delete U early); every object
 * X waits for has a larger serial than X; between two requeues of X
 * there is real work; and X finishes last. */
static void test_reaper_requeue(void)
{
    struct cspace x, y;

    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 2, &uts, 1, 1) == T_OK);        /* X */
    CHECK(cnode_view(&uts, 1, &x) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 14, &x, 0, 1) == T_OK);       /* U in X[0] */
    CHECK(cnode_view(&uts, 1, &x) == T_OK);
    for (cap_slot_t i = 0; i < 3; i++) {                                 /* Y1..Y3 from U */
        CHECK(cap_retype(&x, 0, CAP_CNODE, 1, &uts, 10 + i, 1) == T_OK);
        CHECK(cnode_view(&uts, 10 + i, &y) == T_OK);
        CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &y, 0, 2) == T_OK);       /* real work */
    }
    CHECK(cap_retype(&x, 0, CAP_FRAME, 0, &uts, 20, 1) == T_OK);         /* a live descendant */
    CHECK(cnode_view(&uts, 1, &x) == T_OK);
    struct cap *u = &x.slots[0];
    uint64_t sx = x.slots[0].slot_meta.serial, su = u->ut.serial;
    uint64_t sy[3];
    for (unsigned i = 0; i < 3; i++)
        sy[i] = obj_serial(&ut_store[10 + i]);
    CHECK(sx < su && su < sy[0] && su < sy[1] && su < sy[2]);             /* step 1 */

    reap_trace_n = 0;
    CHECK(cap_delete(&uts, 1) == T_OK);                                  /* X dies */
    /* F7: the first unit freezes U and deletes its first descendant; the
     * holders of the rest cannot derive into U meanwhile. */
    CHECK(reaper_run(1) == 1 && (u->ut.flags & OBJ_FROZEN));
    CHECK(ut_store[20].type == CAP_NULL && ut_store[10].type == CAP_CNODE);  /* first child went */
    CHECK(cap_derive(&uts, 10, 21, CAP_R_ALL, 0) == T_EBUSY && ut_store[21].type == CAP_NULL);
    unsigned units = reaper_drain(100000);
    CHECK(units > 0 && reaper_idle() && ut_store[0].ut.dying == 0 && AUDIT());

    /* The trace against the proof. */
    unsigned requeues = 0, last_x = 0, x_done = 0, y_done = 0;
    bool real_between = true, order_ok = true;
    for (unsigned k = 0; k < reap_trace_n; k++) {
        const struct reap_event *ev = &reap_trace[k];
        if (ev->serial == sx) {
            if (ev->act == REAP_REQUEUE) {
                if (requeues) {
                    /* step 5: real work on a dependency since the last one */
                    bool seen = false;
                    for (unsigned j = last_x + 1; j < k; j++)
                        seen |= reap_trace[j].act == REAP_REAL && reap_trace[j].serial > sx;
                    real_between &= seen;
                }
                requeues++;
                last_x = k;
            } else if (ev->act == REAP_DONE) {
                x_done = k + 1;
            }
        } else {
            order_ok &= ev->serial > sx;          /* step 1: only younger objects */
            if (ev->act == REAP_DONE)
                y_done++;
        }
    }
    CHECK(requeues >= 1 && real_between && order_ok);
    CHECK(x_done == reap_trace_n && y_done == 3);   /* X finishes last, after Y1..Y3 */
    CHECK(reap_trace_n < REAP_TRACE_MAX);           /* the trace saw everything */
    ut_teardown();
}

/* SC21, D3: serials are never reused. Near exhaustion, creation fails
 * with T_ENOMEM and nothing changes; the last serials are still issued. */
static void test_serial_exhaustion(void)
{
    CHECK(ut_setup(20));
    uint64_t saved = objmeta_test_next_serial();
    objmeta_test_set_serial(UINT64_MAX - 2);   /* two serials left */
    world_tables(NULL, 0);
    world_take(&world_a);
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 3) == T_ENOMEM);
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 2) == T_OK);
    CHECK(obj_serial(&ut_store[1]) == UINT64_MAX - 2 && obj_serial(&ut_store[2]) == UINT64_MAX - 1);
    CHECK(cap_retype(&uts, 0, CAP_NOTIFICATION, 0, &uts, 3, 1) == T_ENOMEM);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 12, &uts, 3, 1) == T_ENOMEM);
    CHECK(objmeta_test_next_serial() == UINT64_MAX);   /* never wrapped */
    CHECK(AUDIT());
    ut_teardown();
    /* Test-only: the counter goes back to where it was. No object with a
     * serial at or above `saved` exists, so nothing can collide. */
    objmeta_test_set_serial(saved);
    CHECK(AUDIT());
}

/* ---- Stage 4: revocation as a continuation (ADR 0010 SC22, SC22a,
 * F1-F7; revision 3e E0, E1, E3, E6) ---- */

/* Apply every deletion the kernel made since the last call to the model:
 * a slot that went empty takes itself out, its children re-parented. */
static unsigned big_sync(const struct cspace *cs)
{
    unsigned gone = 0;
    for (int g = 0; g < BIG_N; g++) {
        if (big_par[g] == M_EMPTY || cs->slots[g].type != CAP_NULL)
            continue;
        for (int h = 0; h < BIG_N; h++)
            if (big_par[h] == g)
                big_par[h] = big_par[g];
        big_par[g] = M_EMPTY;
        gone++;
    }
    return gone;
}

static void big_build(struct cspace *cs, uint64_t token)
{
    cspace_init(cs, big_store, BIG_N);
    CHECK(cap_install(cs, 0, CAP_TEST_TOKEN, CAP_R_ALL, token) == T_OK);
    big_par[0] = M_ROOT;
    for (int i = 1; i <= 1500; i++) {                  /* chain 0 > 1 > ... > 1500 */
        CHECK(cap_derive(cs, (cap_slot_t)(i - 1), (cap_slot_t)i, CAP_R_ALL, 0) == T_OK);
        big_par[i] = (int16_t)(i - 1);
    }
    for (int i = 1501; i < BIG_N; i++) {                /* fan under the root */
        CHECK(cap_derive(cs, 0, (cap_slot_t)i, CAP_R_ALL, 0) == T_OK);
        big_par[i] = 0;
    }
}

/* SC22, E1, E6, at the kernel API, chunk by chunk: the mark, the freeze,
 * exactly REVOKE_CHUNK deletions per chunk with the tree equal to the
 * reference forest after each, F4 and F5 refusals, and abandonment. */
static void test_revoke_chunks(void)
{
    struct cspace cs;
    struct thread_cont k = { 0 }, k2 = { 0 };
    struct cap_info info;
    const struct cap *c;

    big_build(&cs, 0x3000);
    CHECK(big_agrees(&cs) && meta_audit());
    CHECK(cap_revoke_begin(&cs, 0, &k) == CONT_PENDING);
    CHECK(k.kind == CONT_REVOKE && k.slot == 0 && k.type == CAP_TEST_TOKEN &&
          k.object == 0x3000 && k.serial == obj_serial(&cs.slots[0]));
    CHECK(cs.slots[0].rights == (CAP_R_ALL | CAP_R_REVOKING));
    CHECK(obj_frozen(&cs.slots[0]) && obj_frozen(&cs.slots[1500]));
    /* E1 properties 1-3: never shown, never copied, the root stays. */
    CHECK(cap_identify(&cs, 0, &info) == T_OK && info.rights == CAP_R_ALL);
    CHECK(cap_lookup(&cs, 0, CAP_TEST_TOKEN, CAP_R_REVOKING, &c) == T_OK);  /* kernel view */
    CHECK(cap_delete(&cs, 0) == T_EBUSY && cap_revoke(&cs, 0, 1) == T_EBUSY);
    /* F4: a second revocation of the object, from any of its records. */
    CHECK(cap_revoke_begin(&cs, 0, &k2) == T_EBUSY && k2.kind == CONT_NONE);
    CHECK(cap_revoke_begin(&cs, 1, &k2) == T_EBUSY && k2.kind == CONT_NONE);
    /* SC22a: nothing derives into the subtree, from any of its records. */
    struct cspace other;
    static struct cap other_store[4];
    cspace_init(&other, other_store, 4);
    CHECK(cap_derive_to(&cs, 1500, &other, 0, CAP_R_ALL, 0) == T_EBUSY);
    CHECK(cap_derive_to(&cs, 0, &other, 0, CAP_R_ALL, 0) == T_EBUSY);
    CHECK(meta_audit() == false);   /* a mark with no owning thread: the audit sees it */

    unsigned left = BIG_N - 1, chunks = 0;
    bool exact = true, agrees = true;
    int rc;
    while ((rc = cap_revoke_chunk(&cs, &k)) == CONT_PENDING) {
        unsigned gone = big_sync(&cs);
        exact &= gone == REVOKE_CHUNK;
        left -= gone;
        chunks++;
        if (chunks % 32 == 0)
            agrees &= big_agrees(&cs);
        exact &= obj_frozen(&cs.slots[0]) && (cs.slots[0].rights & CAP_R_REVOKING);
    }
    unsigned last = big_sync(&cs);
    left -= last;
    CHECK(rc == T_OK && exact && agrees && last <= REVOKE_CHUNK && left == 0);
    CHECK(chunks == (BIG_N - 1) / REVOKE_CHUNK);
    /* Done: the mark and the freeze are gone with the operation. */
    CHECK(cs.slots[0].rights == CAP_R_ALL && !obj_frozen(&cs.slots[0]));
    CHECK(!cap_has_children(&cs.slots[0]) && meta_audit());

    /* Abandonment (F6): released, deleted records stay deleted, and the
     * continuation can never act again (E1 property 7). */
    cspace_destroy(&cs);
    big_build(&cs, 0x3001);
    CHECK(cap_revoke_begin(&cs, 0, &k) == CONT_PENDING);
    CHECK(cap_revoke_chunk(&cs, &k) == CONT_PENDING && cap_revoke_chunk(&cs, &k) == CONT_PENDING);
    CHECK(big_sync(&cs) == 2 * REVOKE_CHUNK);
    cap_revoke_abandon(&cs, &k);
    CHECK(cs.slots[0].rights == CAP_R_ALL && !obj_frozen(&cs.slots[0]) && meta_audit());
    CHECK(cap_revoke_chunk(&cs, &k) == T_EINTR && big_sync(&cs) == 0);
    CHECK(big_agrees(&cs));
    /* E1 property 8: a later record in the root's slot, even of the same
     * object, is never taken for the root. */
    CHECK(cap_derive(&cs, 1600, 3071, CAP_R_ALL, 0) == T_OK || cs.slots[3071].type != CAP_NULL);
    cspace_destroy(&cs);
    big_build(&cs, 0x3002);
    CHECK(cap_revoke_begin(&cs, 1600, &k) == CONT_PENDING || !cap_has_children(&cs.slots[1600]));
    CHECK(cap_revoke_begin(&cs, 0, &k) == CONT_PENDING);
    cap_revoke_abandon(&cs, &k);
    CHECK(cap_derive(&cs, 1600, 0, CAP_R_ALL, 0) == T_EBUSY);   /* slot 0 occupied */
    CHECK(cap_delete(&cs, 1) == T_OK);                          /* a different record */
    CHECK(cap_derive(&cs, 0, 1, CAP_R_ALL, 0) == T_OK);         /* same object, slot 1 */
    struct thread_cont k1 = k;
    k1.slot = 1;                                                /* aimed at it */
    CHECK(cap_revoke_chunk(&cs, &k1) == T_EINTR);               /* no mark: never the root */
    cspace_destroy(&cs);
    CHECK(meta_audit());
}

/* F4, F5: an enclosing revocation, and the reaper, cancel an inner one
 * without waiting for it; the inner one then ends with T_EINTR. */
static void test_revoke_cancel(void)
{
    struct thread_cont in = { 0 }, out = { 0 };
    struct cspace cn;

    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &uts, 20, 1) == T_OK);   /* U1 */
    CHECK(cap_retype(&uts, 20, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);   /* E under U1 */
    CHECK(cap_derive(&uts, 1, 2, CAP_R_ALL, 0) == T_OK && cap_derive(&uts, 2, 3, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_revoke_begin(&uts, 1, &in) == CONT_PENDING);              /* inner: E */
    /* A revocation under the frozen object, and one of a descendant, are
     * refused; the enclosing Untyped's is not (F4). */
    CHECK(cap_revoke_begin(&uts, 2, &out) == T_EBUSY);
    CHECK(cap_revoke_begin(&uts, 20, &out) == CONT_PENDING);            /* outer: U1 */
    CHECK(obj_frozen(&uts.slots[1]));   /* both: E's own flag, and U1 on its chain */
    int rc;
    while ((rc = cap_revoke_chunk(&uts, &out)) == CONT_PENDING)
        ;
    CHECK(rc == T_OK && uts.slots[1].type == CAP_NULL && uts.slots[2].type == CAP_NULL);
    CHECK(!(uts.slots[20].ut.flags & OBJ_FROZEN) && !(uts.slots[20].rights & CAP_R_REVOKING));
    CHECK(cap_revoke_chunk(&uts, &in) == T_EINTR);                      /* cancelled */
    reap_all();
    CHECK(meta_audit());

    /* The reaper meets a marked root in a dying CNode: it cancels it. */
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 2, &uts, 30, 1) == T_OK);
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 31, 1) == T_OK);
    CHECK(cnode_view(&uts, 30, &cn) == T_OK);
    CHECK(cap_derive_to(&uts, 31, &cn, 0, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive_to(&cn, 0, &uts, 32, CAP_R_ALL, 0) == T_OK);       /* a child elsewhere */
    CHECK(cap_revoke_begin(&cn, 0, &in) == CONT_PENDING && obj_frozen(&uts.slots[31]));
    CHECK(cap_delete(&uts, 30) == T_OK);                                /* the CNode dies */
    reap_all();
    CHECK(!obj_frozen(&uts.slots[31]) && uts.slots[32].type == CAP_ENDPOINT);  /* child kept */
    CHECK(cap_revoke_chunk(&cn, &in) == T_EINTR);
    CHECK(meta_audit());
    ut_teardown();
}

/* A subtree for user-level revocation: the root is in runner b's cspace
 * slot 2 (an endpoint, uts 1), its descendants in big_store. */
static void revoke_subtree(cap_slot_t b, struct cspace *bs, int n)
{
    struct cspace c = task_cs(b);

    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    CHECK(cap_derive_to(&uts, 1, &c, 2, CAP_R_ALL, 0) == T_OK);
    cspace_init(bs, big_store, BIG_N);
    CHECK(cap_derive_to(&c, 2, bs, 0, CAP_R_ALL, 0) == T_OK);
    for (int i = 1; i < n; i++)
        CHECK(cap_derive(bs, (cap_slot_t)(i % 2 ? i - 1 : 0), (cap_slot_t)i, CAP_R_ALL, 0) == T_OK);
}

static unsigned live_in(const struct cspace *cs, int n)
{
    unsigned k = 0;
    for (int i = 0; i < n; i++)
        k += cs->slots[i].type != CAP_NULL;
    return k;
}

/* SC22 through a user thread: the syscall runs to completion over many
 * dispatches, while other threads keep trying to derive into the
 * subtree; and every way of stopping the revoker mid-way releases the
 * freeze (F6, E1), keeps what was deleted, and leaves T_EINTR. */
static void test_revoke_syscall(void)
{
    struct cspace bs;
    enum { N = 600 };

    /* To completion, with derivers interleaved. */
    CHECK(ut_setup(20));
    CHECK(spawn(10, P(runner), false, NULL) && spawn(20, P(runner), false, NULL));
    CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK && thread_suspend(&uts, 20 + T_THR) == T_OK);
    revoke_subtree(10, &bs, N);
    struct cspace c20 = task_cs(20);
    CHECK(cap_derive_to(&bs, N - 1, &c20, 2, CAP_R_ALL, 0) == T_OK);   /* a holder inside */
    nreq = 0;
    REQ(10, SYS_CAP_REVOKE, 2);
    nreq = 0;
    for (unsigned i = 0; i < 20; i++) {
        REQ(20, SYS_YIELD);
        REQ(20, SYS_CAP_DERIVE, 2, 3 + i % 8, CAP_R_ALL, 0);
    }
    uint64_t chunks = cont_chunks_run;
    CHECK(thread_resume(&uts, 10 + T_THR) == T_OK && thread_resume(&uts, 20 + T_THR) == T_OK);
    run_user();
    CHECK(res(10, 0) == T_OK && live_in(&bs, N) == 0);
    CHECK(cont_chunks_run - chunks >= N / REVOKE_CHUNK);   /* many dispatches, not one */
    struct cspace c10 = task_cs(10);
    CHECK(c10.slots[2].rights == CAP_R_ALL && !obj_frozen(&c10.slots[2]));
    bool refused = true;
    for (unsigned i = 0; i < 20; i++)
        refused &= res(20, 2 * i + 1) == T_EBUSY || res(20, 2 * i + 1) == T_EBADCAP;
    for (cap_slot_t s = 3; s < 11; s++)
        refused &= c20.slots[s].type == CAP_NULL;
    CHECK(refused && tstate(10) == THREAD_INACTIVE && AUDIT());
    ut_teardown();

    /* Stopped mid-way, three ways: suspend, unbind the cspace, destroy. */
    for (int how = 0; how < 3; how++) {
        CHECK(ut_setup(20));
        CHECK(spawn(10, P(runner), false, NULL) && spawn(30, P(runner), false, NULL));
        CHECK(thread_suspend(&uts, 10 + T_THR) == T_OK && thread_suspend(&uts, 30 + T_THR) == T_OK);
        revoke_subtree(10, &bs, N);
        struct cspace c30 = task_cs(30);
        CHECK(cap_derive_to(&uts, 10 + T_THR, &c30, 2, CAP_R_ALL, 0) == T_OK);
        if (how == 2)
            CHECK(cap_delete(&uts, 10 + T_THR) == T_OK);   /* c30's copy is now the last */
        nreq = 0;
        REQ(10, SYS_CAP_REVOKE, 2);
        nreq = 0;
        REQ(30, SYS_YIELD);
        REQ(30, SYS_YIELD);
        if (how == 0)
            REQ(30, SYS_TH_SUSPEND, 2);
        else if (how == 1)
            REQ(30, SYS_TH_UNBIND, 2, TS_CSPACE);
        else
            REQ(30, SYS_CAP_DELETE, 2);
        CHECK(thread_resume(&uts, 10 + T_THR) == T_OK || how == 2);
        if (how == 2)
            CHECK(thread_resume(&c30, 2) == T_OK);
        CHECK(thread_resume(&uts, 30 + T_THR) == T_OK);
        run_user();
        unsigned left = live_in(&bs, N);
        /* Some deleted, some left: stopped mid-way, nothing rolled back. */
        CHECK(left > 0 && left < N);
        struct cspace r10 = task_cs(10);
        CHECK(r10.slots[2].rights == CAP_R_ALL && !obj_frozen(&r10.slots[2]));  /* released */
        CHECK(AUDIT());
        if (how == 0) {
            /* Resumed: the interrupted syscall returns T_EINTR, and a new
             * revocation finishes the job. */
            CHECK(tstate(10) == THREAD_INACTIVE && th(10)->cont.kind == CONT_NONE);
            CHECK(th(10)->regs.rax == (uint64_t)(int64_t)T_EINTR);
            CHECK(thread_resume(&uts, 10 + T_THR) == T_OK);
            run_user();
            CHECK(res(10, 0) == T_EINTR);
            CHECK(cap_revoke(&r10, 2, 0) == T_OK && live_in(&bs, N) == 0);
        }
        cspace_destroy(&bs);
        ut_teardown();
        CHECK(AUDIT());
    }
}

/* SC28: the retype limit, at its exact boundary. Every request names few
 * destination slots, so no other check can be what refuses it. */
static void test_retype_limit(void)
{
    CHECK(ut_setup(22));
    world_tables(NULL, 0);
    world_take(&world_a);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 11, &uts, 1, 3) == T_ERANGE);    /* 768 KiB */
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 12, &uts, 1, 2) == T_ERANGE);    /* 1 MiB */
    world_take(&world_b);
    CHECK(world_same(&world_a, &world_b));
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 11, &uts, 1, 2) == T_OK);        /* exactly 512 KiB */
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 12, &uts, 3, 1) == T_OK);        /* the largest object */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 20, &uts, 4, 1) == T_OK);      /* not zeroed: no limit */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 5, 100) == T_OK);       /* 400 KiB */
    ut_teardown();
}

/* R6, directly: the thread that causes a death is stopped (suspends
 * itself) or destroyed (deletes its own last Thread capability) in its
 * very next syscall, before any scheduling boundary has let the reaper
 * run. The teardown is unaffected: every waiter failed exactly once,
 * the pin dropped, the memory reusable. */
static void test_reaper_r6(void)
{
    for (int how = 0; how < 2; how++) {
        CHECK(ut_setup(20));
        CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &uts, 100, 1) == T_OK);   /* Ua */
        CHECK(cap_retype(&uts, 100, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
        struct endpoint *e = endpoint_of(&ut_store[1]);
        const cap_slot_t w[] = { 10, 20, 30, 40, 50, 60 };   /* > REAP_CHUNK */
        for (unsigned i = 0; i < ARRAY_SIZE(w); i++)
            CHECK(ep_thread(w[i], false, 0, 0, CAP_R_READ));
        run_user();
        CHECK(spawn(70, P(runner), false, NULL) && thread_suspend(&uts, 70 + T_THR) == T_OK);
        struct cspace c70 = task_cs(70);
        CHECK(cap_derive_to(&uts, 1, &c70, 2, CAP_R_ALL, 0) == T_OK);
        CHECK(cap_derive_to(&uts, 70 + T_THR, &c70, 3, CAP_R_ALL, 0) == T_OK);
        for (unsigned i = 0; i < ARRAY_SIZE(w); i++) {
            struct cspace c = task_cs(w[i]);
            CHECK(cap_delete(&c, 1) == T_OK);
        }
        CHECK(cap_delete(&uts, 1) == T_OK);               /* the deleter's is the last */
        nreq = 0;
        REQ(70, SYS_CAP_DELETE, 2);                       /* the death */
        if (how == 0) {
            REQ(70, SYS_TH_SUSPEND, 3);                   /* stopped, at once */
        } else {
            CHECK(thread_resume(&uts, 70 + T_THR) == T_OK);
            CHECK(cap_delete(&uts, 70 + T_THR) == T_OK);  /* c70[3] is now the last */
            REQ(70, SYS_CAP_DELETE, 3);                   /* destroyed, at once */
        }
        if (how == 0)
            CHECK(thread_resume(&uts, 70 + T_THR) == T_OK);
        reap_trace_n = 0;
        test_report_count = 0;
        uint64_t units = reaper_units_total;
        run_user();
        if (how == 0)
            CHECK(tstate(70) == THREAD_INACTIVE);
        else
            CHECK(ut_store[70 + T_THR].type == CAP_NULL && c70.slots[3].type == CAP_NULL);
        /* The deleter was stopped before the reaper's first unit (the last
         * thread_stop of the run is its own; the waiters exit by
         * SYS_EXIT), and the reaper did all the work after. */
        CHECK(thread_stop_reaper_units == units && reaper_units_total > units);
        CHECK(reap_trace_n > 0 && reaper_idle() && ut_store[100].ut.dying == 0);
        CHECK(e->meta.flags == 0 && e->meta.serial == 0);   /* retired */
        CHECK(reports_with(T_EDEAD) == ARRAY_SIZE(w));      /* each waiter once */
        CHECK(cap_retype(&uts, 100, CAP_FRAME, 0, &uts, 101, 16) == T_OK);  /* reusable */
        CHECK(slot_paddr(101) == slot_paddr(100) && AUDIT());
        ut_teardown();
    }
}

/* ---- Stage 4: the receive drain as a continuation (ADR 0010 SC25;
 * revision 3e E0, E2, E7) ---- */

/* Senders sharing task `base`'s address space, code (`resend`) and
 * cspace, each with only a thread object (uts slot thr) and its own IPC
 * buffer frame (thr + 1): two slots per sender. Each sends `info` with
 * capabilities 2 and 3 of the shared cspace, `times` times. */
static bool light_sender(cap_slot_t base, cap_slot_t thr, uint64_t info, uint64_t times)
{
    struct user_regs r = { .rip = UCODE_VA, .rsp = USTACK_TOP, .r12 = SYS_SEND, .rdi = 1,
                           .rsi = info, .r13 = times };
    if (cap_retype(&uts, 0, CAP_THREAD, 0, &uts, thr, 1) != T_OK ||
        cap_retype(&uts, 0, CAP_FRAME, 0, &uts, thr + 1, 1) != T_OK ||
        thread_bind(&uts, thr, TS_ASPACE, &uts, base + T_AS) != T_OK ||
        thread_bind(&uts, thr, TS_CSPACE, &uts, base + T_CS) != T_OK ||
        thread_bind(&uts, thr, TS_IPCBUF, &uts, thr + 1) != T_OK)
        return false;
    struct ipc_buffer *b = phys_to_virt(cap_obj_paddr(&ut_store[thr + 1]));
    b->caps[0] = 2;
    b->caps[1] = 3;
    return thread_write_regs(&uts, thr, &r) == T_OK && thread_resume(&uts, thr) == T_OK;
}

/* The shared task for light senders: `resend` at base, sending on the
 * endpoint in uts slot `ep` (its cspace slot 1), with capabilities 2 and
 * 3 to send. It is itself the first sender. */
static bool sender_base(cap_slot_t base, cap_slot_t ep, uint64_t info, uint64_t times)
{
    struct user_regs r = { .r12 = SYS_SEND, .rdi = 1, .rsi = info, .r13 = times };
    if (!spawn(base, P(resend), false, &r))
        return false;
    struct cspace cs = task_cs(base);
    tbuf(base)->caps[0] = 2;
    tbuf(base)->caps[1] = 3;
    return cap_derive_to(&uts, ep, &cs, 1, CAP_R_WRITE | G_, 0) == T_OK &&
           cap_derive_to(&uts, base + T_CODE, &cs, 2, CAP_R_READ | G_, 0) == T_OK &&
           cap_derive_to(&uts, base + T_STACK, &cs, 3, CAP_R_READ | G_, 0) == T_OK &&
           thread_bind(&uts, base + T_THR, TS_IPCBUF, &uts, base + T_DATA) == T_OK;
}

#define TWO_CAPS (MSG_INFO(9, 0) | CAPS(2))   /* undeliverable into a 1-slot offer */
static struct ipc_buffer *drain_rbuf;
static int drain_hook_calls;

static void hook_rewrite_offer(struct thread *t)
{
    (void)t;
    drain_hook_calls++;
    drain_rbuf->recv_slots[0] = 12;   /* rewritten in user memory: no effect */
    drain_rbuf->recv_count = 3;
}

/* SC25, E7: more than DRAIN_CHUNK undeliverable senders, then a
 * deliverable one. Same outcome as one pass; each failed sender failed
 * exactly once; spread over chunks; the captured offer is what counts,
 * whatever user memory says later. */
static void test_drain_chunks(void)
{
    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 1) == T_OK);
    struct endpoint *e = endpoint_of(&ut_store[1]);
    CHECK(sender_base(10, 1, TWO_CAPS, 1));
    for (cap_slot_t i = 0; i < 9; i++)                     /* 10 undeliverable */
        CHECK(light_sender(10, 60 + 2 * i, TWO_CAPS, 1));
    run_user();
    CHECK(e->senders.len == 10);
    CHECK(xfer_sender(30, 7, 0x77, (const uint32_t[]){ 2 }, 1));   /* deliverable */
    run_user();
    CHECK(e->senders.len == 11 && e->senders.tail == th(30));
    drain_rbuf = ep_server(40, 50, CAP_R_READ | G_);
    drain_rbuf->recv_slots[0] = 9;
    drain_rbuf->recv_count = 1;
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    uint64_t chunks = cont_chunks_run, failed = drain_failed;
    drain_hook_calls = 0;
    cont_test_hook = hook_rewrite_offer;
    test_report_count = 0;
    go(40);
    cont_test_hook = NULL;
    /* 10 failures: DRAIN_CHUNK in the syscall's chunk, the rest after one
     * boundary. */
    CHECK(drain_failed - failed == 10 && cont_chunks_run - chunks == 1 && drain_hook_calls == 1);
    CHECK(res(40, 0) == T_OK && drain_rbuf->words[0] == 0x77);
    struct cspace c40 = task_cs(40);
    CHECK(c40.slots[9].type == CAP_ENDPOINT && c40.slots[12].type == CAP_NULL);   /* the captured offer */
    CHECK(reports_with(T_EINVAL) == 10 && test_report_count == 11);           /* each failed once */
    CHECK(!e->senders.head && ep_ok(e) && AUDIT());
    ut_teardown();
}

static cap_slot_t aba_slot;
static void hook_swap_endpoint(struct thread *t)
{
    struct cspace cs;
    (void)t;
    drain_hook_calls++;
    /* Between chunks: the endpoint the receive named goes from its slot,
     * and a different one takes the slot. */
    CHECK(cnode_view(&uts, 40 + T_CS, &cs) == T_OK);
    CHECK(cap_delete(&cs, 1) == T_OK);
    if (aba_slot)
        CHECK(cap_derive_to(&uts, aba_slot, &cs, 1, CAP_R_READ | G_, 0) == T_OK);
}

/* E0 for the drain: the endpoint deleted between chunks, alone or with a
 * new endpoint put in the same slot. The receive ends with the lookup
 * error and never acts on the new endpoint; what earlier chunks
 * committed (failed senders) stands. */
static void test_drain_identity(void)
{
    for (int with_new = 0; with_new < 2; with_new++) {
        CHECK(ut_setup(20));
        CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 2) == T_OK);   /* E (1), E' (2) */
        struct endpoint *e = endpoint_of(&ut_store[1]), *e2 = endpoint_of(&ut_store[2]);
        CHECK(sender_base(10, 1, TWO_CAPS, 1));
        for (cap_slot_t i = 0; i < 9; i++)
            CHECK(light_sender(10, 60 + 2 * i, TWO_CAPS, 1));
        CHECK(xfer_sender(30, 7, 0x77, (const uint32_t[]){ 2 }, 1));        /* on E too */
        run_user();
        CHECK(e->senders.len == 11);
        drain_rbuf = ep_server(40, 50, CAP_R_READ | G_);
        drain_rbuf->recv_slots[0] = 9;
        drain_rbuf->recv_count = 1;
        nreq = 0;
        REQ(40, SYS_RECV, 1);
        struct obj_meta m2 = e2->meta;
        aba_slot = with_new ? 2 : 0;
        drain_hook_calls = 0;
        cont_test_hook = hook_swap_endpoint;
        go(40);
        cont_test_hook = NULL;
        CHECK(drain_hook_calls == 1);
        CHECK(res(40, 0) == T_EBADCAP);   /* gone, or not the one named */
        /* Eight failed in the first chunk, and stay failed; the rest,
         * the deliverable one included, are still queued on E. */
        CHECK(e->senders.len == 3 && e->senders.tail == th(30) && tstate(30) == THREAD_BLOCKED_SEND);
        /* E' was never touched: no sender delivered or failed through it,
         * no waiter, counts only the new copy's. */
        CHECK(!e2->senders.head && !e2->receivers.head);
        CHECK(e2->meta.live == m2.live + (with_new ? 1 : 0));
        CHECK(th(40)->cont.kind == CONT_NONE && th(40)->offer.count == 0 && AUDIT());
        ut_teardown();
    }
}

/* E2: senders that keep coming back (each failed sender re-sends) cannot
 * delay the caller's reply beyond the k senders queued when reply_recv
 * began; the rest is the server's own receive work. */
static void test_drain_reply_bound(void)
{
    enum { K = 10, TIMES = 4 };

    CHECK(ut_setup(20));
    CHECK(cap_retype(&uts, 0, CAP_ENDPOINT, 0, &uts, 1, 2) == T_OK);   /* E1 (1), E2 (2) */
    struct endpoint *e2 = endpoint_of(&ut_store[2]);
    /* The caller, on E1, blocked waiting for a server. */
    CHECK(ep_caller(20, 5, 0x55, CAP_R_WRITE | G_, 0, (const uint32_t[]){ 6 }, 1));
    run_user();
    CHECK(tstate(20) == THREAD_BLOCKED_SEND);
    /* K undeliverable senders on E2, each coming back TIMES times. */
    CHECK(sender_base(10, 2, TWO_CAPS, TIMES));
    for (cap_slot_t i = 0; i < K - 1; i++)
        CHECK(light_sender(10, 60 + 2 * i, TWO_CAPS, TIMES));
    run_user();
    CHECK(e2->senders.len == K);
    /* The server: receives the call on E1, then reply_recv on E2 with a
     * one-slot offer. */
    drain_rbuf = ep_server(40, 50, CAP_R_READ | G_);
    struct cspace c40 = task_cs(40);
    CHECK(cap_derive_to(&uts, 2, &c40, 2, CAP_R_READ | G_, 0) == T_OK);
    drain_rbuf->recv_slots[0] = 9;
    drain_rbuf->recv_count = 1;
    nreq = 0;
    REQ(40, SYS_RECV, 1);
    REQ(40, SYS_REPLY_RECV, 2, MSG_INFO(0x66, 0));
    uint64_t failed = drain_failed;
    drain_failed_at_reply = 0;
    test_report_count = 0;
    go(40);
    /* The reply committed after exactly the K senders of the snapshot,
     * although they kept coming back throughout. */
    CHECK(drain_failed_at_reply - failed == K);
    CHECK(drain_failed - failed == K * TIMES);           /* every attempt failed once */
    CHECK(tstate(20) == THREAD_INACTIVE && reports_with(T_OK) >= 1);   /* the caller got it */
    CHECK(res(40, 0) == T_OK && tstate(40) == THREAD_BLOCKED_RECV);    /* now just receiving */
    CHECK(!e2->senders.head && AUDIT());
    ut_teardown();
}

static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {
    { "printf", test_printf },
    { "string", test_string },
    { "overflow_helpers", test_overflow_helpers },
    { "pmm", test_pmm },
    { "capabilities", test_caps },
    { "cap_revoke", test_cap_revoke },
    { "untyped_ownership", test_untyped_ownership },
    { "untyped_rules", test_untyped_rules },
    { "untyped_exhaustion", test_untyped_exhaustion },
    { "untyped_overlap", test_untyped_overlap },
    { "untyped_revoke", test_untyped_revoke },
    { "cdt_forest", test_cdt_forest },
    { "cdt_random", test_cdt_random },
    { "cdt_untyped_descent", test_cdt_untyped_descent },
    { "cdt_revoke_bound", test_cdt_revoke_bound },
    { "cnode_chain", test_cnode_chain },
    { "vspace_perms", test_vspace_perms },
    { "vspace_alias", test_vspace_alias },
    { "vspace_deputy", test_vspace_deputy },
    { "vspace_teardown", test_vspace_teardown },
    { "w^x", test_wx },
    { "stack_guards", test_stack_guards },
    { "percpu_fpu", test_percpu_fpu },
    { "thread_regs", test_thread_regs },
    { "thread_lifecycle", test_thread_lifecycle },
    { "user_basics", test_user_basics },
    { "user_entry_state", test_user_entry_state },
    { "user_faults", test_user_faults },
    { "user_sysret_edge", test_user_sysret_edge },
    { "uaccess", test_uaccess },
    { "syscalls", test_syscalls },
    { "syscall_lifecycle", test_syscall_lifecycle },
    { "entry_paths", test_entry_paths },
    { "ipc_audit_detects", test_ipc_audit_detects },
    { "ipc_states", test_ipc_states },
    { "ipc_reply_links", test_ipc_reply_links },
    { "notif_semantics", test_notif_semantics },
    { "notif_badges", test_notif_badges },
    { "notif_waiters", test_notif_waiters },
    { "notif_liveness", test_notif_liveness },
    { "notif_destroy", test_notif_destroy },
    { "ep_fifo", test_ep_fifo },
    { "ep_format", test_ep_format },
    { "ep_capture", test_ep_capture },
    { "ep_liveness", test_ep_liveness },
    { "ep_destroy", test_ep_destroy },
    { "ep_ipcbuf_wx", test_ep_ipcbuf_wx },
    { "ep_xfer_basic", test_ep_xfer_basic },
    { "ep_xfer_refuse", test_ep_xfer_refuse },
    { "ep_xfer_collision", test_ep_xfer_collision },
    { "ep_xfer_queued", test_ep_xfer_queued },
    { "ep_xfer_drain", test_ep_xfer_drain },
    { "ep_badge", test_ep_badge },
    { "call_basic", test_call_basic },
    { "call_offer", test_call_offer },
    { "reply_refuse", test_reply_refuse },
    { "reply_gone", test_reply_gone },
    { "reply_recv", test_reply_recv },
    { "reply_recv_edges", test_reply_recv_edges },
    { "fault_deliver", test_fault_deliver },
    { "fault_authority", test_fault_authority },
    { "fault_nohandler", test_fault_nohandler },
    { "fault_handler_gone", test_fault_handler_gone },
    { "fault_self", test_fault_self },
    { "fault_nested", test_fault_nested },
    { "fault_kernel_paths", test_fault_kernel_paths },
    { "fault_contract", test_fault_contract },
    { "meta_counts", test_meta_counts },
    { "meta_frames", test_meta_frames },
    { "meta_overflow", test_meta_overflow },
    { "meta_freeze", test_meta_freeze },
    { "reaper_dying", test_reaper_dying },
    { "reaper_aba", test_reaper_aba },
    { "reaper_requeue", test_reaper_requeue },
    { "serial_exhaustion", test_serial_exhaustion },
    { "revoke_chunks", test_revoke_chunks },
    { "revoke_cancel", test_revoke_cancel },
    { "revoke_syscall", test_revoke_syscall },
    { "retype_limit", test_retype_limit },
    { "drain_chunks", test_drain_chunks },
    { "drain_identity", test_drain_identity },
    { "drain_reply_bound", test_drain_reply_bound },
    { "reaper_r6", test_reaper_r6 },
    { "cpu_hardening", test_cpu_hardening },
};

__noreturn void selftest_run(void)
{
    kprintf("selftest: running %zu tests\n", ARRAY_SIZE(tests));

    for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
        unsigned before = failures;
        tests[i].fn();
        CHECK(meta_audit());   /* SC31.3: counters exact after every test */
        kprintf("  [%s] %s\n", failures == before ? "PASS" : "FAIL", tests[i].name);
    }

    /* T33: measured worst-case use of every kernel stack, after the
     * whole suite has run. */
    for (size_t i = 0; i < kstack_count; i++) {
        size_t hw = stack_high_water(&kstacks[i]);
        kprintf("  stack %-12s high water %5zu / budget %5zu bytes\n", kstacks[i].name, hw,
                kstacks[i].budget);
        CHECK(hw <= kstacks[i].budget);
    }

    kprintf("selftest: %u checks, %u failures\n", checks, failures);
    if (failures) {
        kprintf("SELFTEST FAILED\n");
        qemu_exit(QEMU_EXIT_FAIL);
    }
    kprintf("SELFTEST PASSED\n");
    qemu_exit(QEMU_EXIT_PASS);
}

#endif
