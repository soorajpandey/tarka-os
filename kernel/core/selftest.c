/*
 * Boot-time kernel self-tests, built only with `make test`. Each test is a
 * plain function; CHECK() records failures without stopping the run so one
 * boot reports every broken invariant.
 */

#ifdef TARKA_SELFTEST

#include <tarka/selftest.h>

#include <tarka/cap.h>
#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/pmm.h>
#include <tarka/string.h>
#include <tarka/untyped.h>
#include <tarka/vspace.h>

#include "cap_internal.h"

#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/paging.h"
#include "../arch/x86_64/percpu.h"
#include "../arch/x86_64/qemu.h"
#include "../arch/x86_64/stacks.h"

static unsigned checks, failures;

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

static void test_caps(void)
{
    struct cap storage[8];
    struct cspace cs;
    const struct cap *c;

    cspace_init(&cs, storage, ARRAY_SIZE(storage));

    CHECK(cap_install(&cs, 0, CAP_ENDPOINT, CAP_R_READ | CAP_R_WRITE | CAP_R_GRANT, 0x1000) == T_OK);
    CHECK(cap_install(&cs, 0, CAP_ENDPOINT, CAP_R_READ, 0x2000) == T_EBUSY);
    CHECK(cap_install(&cs, 8, CAP_ENDPOINT, CAP_R_READ, 0) == T_ERANGE);
    CHECK(cap_install(&cs, 1, CAP_NULL, CAP_R_READ, 0) == T_EINVAL);
    CHECK(cap_install(&cs, 1, CAP_ENDPOINT, 1u << 31, 0) == T_EINVAL);

    /* Lookup enforces type and rights. */
    CHECK(cap_lookup(&cs, 0, CAP_ENDPOINT, CAP_R_WRITE, &c) == T_OK && c->object == 0x1000);
    CHECK(cap_lookup(&cs, 0, CAP_IRQ, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 0, CAP_ENDPOINT, CAP_R_EXEC, &c) == T_EPERM);
    CHECK(cap_lookup(&cs, 5, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 0xFFFFFFFFu, CAP_ENDPOINT, 0, &c) == T_ERANGE);

    /* Derivation can only narrow rights, never widen them. */
    CHECK(cap_derive(&cs, 0, 1, CAP_R_READ | CAP_R_EXEC, 0) == T_OK);
    CHECK(cap_lookup(&cs, 1, CAP_ENDPOINT, CAP_R_READ, &c) == T_OK);
    CHECK(cap_lookup(&cs, 1, CAP_ENDPOINT, CAP_R_EXEC, &c) == T_EPERM);
    CHECK(cap_lookup(&cs, 1, CAP_ENDPOINT, CAP_R_WRITE, &c) == T_EPERM);

    /* Without GRANT a capability cannot be copied onward. */
    CHECK(cap_derive(&cs, 1, 2, CAP_R_ALL, 0) == T_EPERM);
    CHECK(cap_derive(&cs, 0, 1, CAP_R_READ, 0) == T_EBUSY);

    /* Badges are write-once. */
    CHECK(cap_derive(&cs, 0, 3, CAP_R_ALL, 0xB1) == T_OK);
    CHECK(cap_lookup(&cs, 3, CAP_ENDPOINT, 0, &c) == T_OK && c->badge == 0xB1);
    CHECK(cap_derive(&cs, 3, 4, CAP_R_ALL, 0xB2) == T_EPERM);
    CHECK(cap_derive(&cs, 3, 4, CAP_R_READ, 0) == T_OK);
    CHECK(cap_lookup(&cs, 4, CAP_ENDPOINT, 0, &c) == T_OK && c->badge == 0xB1);

    CHECK(cap_delete(&cs, 1) == T_OK);
    CHECK(cap_delete(&cs, 1) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 1, CAP_ENDPOINT, 0, &c) == T_EBADCAP);

    /* cap_identify exposes type, rights and badge only. */
    struct cap_info info;
    memset(&info, 0xEE, sizeof(info));
    CHECK(cap_identify(&cs, 3, &info) == T_OK);
    CHECK(info.type == CAP_ENDPOINT && info.rights == (CAP_R_READ | CAP_R_WRITE | CAP_R_GRANT) &&
          info.badge == 0xB1);
    CHECK(cap_identify(&cs, 1, &info) == T_EBADCAP);

    cspace_destroy(&cs);
}

/* Structural check of every CDT link reachable from a cspace: links are
 * symmetric, depth grows by at most one per step, and empty slots are fully
 * zeroed (no stale links or data). */
static bool cdt_consistent(const struct cspace *cs)
{
    static const struct cap zero;

    for (uint32_t i = 0; i < cs->nslots; i++) {
        const struct cap *c = &cs->slots[i];
        if (c->type == CAP_NULL) {
            if (memcmp(c, &zero, sizeof(zero)) != 0)
                return false;
            continue;
        }
        if (c->reserved != 0)
            return false;
        if (c->prev && c->prev->next != c)
            return false;
        if (c->next && (c->next->prev != c || c->next->depth > c->depth + 1))
            return false;
        if (!c->prev && c->depth != 0)
            return false;
    }
    return true;
}

static bool links_within(const struct cspace *cs, const struct cap *lo, const struct cap *hi)
{
    for (uint32_t i = 0; i < cs->nslots; i++) {
        const struct cap *c = &cs->slots[i];
        if (c->prev && (c->prev < lo || c->prev >= hi))
            return false;
        if (c->next && (c->next < lo || c->next >= hi))
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
    cap_set_finaliser(CAP_ENDPOINT, count_finaliser);

    /* root(a0) ─┬─ a3
     *           └─ a1 ─┬─ b0   (delegated into another cspace)
     *                  └─ a2                                      */
    CHECK(cap_install(&a, 0, CAP_ENDPOINT, CAP_R_ALL, 0xE1) == T_OK);
    CHECK(cap_derive(&a, 0, 1, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive(&a, 1, 2, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive_to(&a, 1, &b, 0, CAP_R_READ | CAP_R_GRANT, 0x77) == T_OK);
    CHECK(cap_derive(&a, 0, 3, CAP_R_READ, 0) == T_OK);
    CHECK(cdt_consistent(&a) && cdt_consistent(&b));
    CHECK(cdt_store_a[1].depth == 1 && cdt_store_a[2].depth == 2 && cdt_store_b[0].depth == 2);

    /* Revoking a1 removes its descendants in both cspaces, keeps a1, and
     * frees nothing because the object is still referenced. */
    CHECK(cap_revoke(&a, 1, 0) == T_OK);
    CHECK(cap_lookup(&a, 2, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&b, 0, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&a, 1, CAP_ENDPOINT, CAP_R_ALL, &c) == T_OK);
    CHECK(cap_lookup(&a, 3, CAP_ENDPOINT, CAP_R_READ, &c) == T_OK);
    CHECK(finalised == 0);
    CHECK(cdt_consistent(&a) && cdt_consistent(&b));

    /* Deleting a parent re-parents its children rather than orphaning
     * them: a later revoke of the grandparent still reaches them. */
    CHECK(cap_derive(&a, 1, 4, CAP_R_ALL, 0) == T_OK);   /* a0 > a1 > a4 */
    CHECK(cap_delete(&a, 1) == T_OK);                     /* a0 > a4 */
    CHECK(cdt_store_a[4].depth == 1);
    CHECK(cap_revoke(&a, 0, 0) == T_OK);
    CHECK(cap_lookup(&a, 4, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&a, 3, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(finalised == 0);

    /* Deleting the last capability runs the finaliser exactly once. */
    CHECK(cap_delete(&a, 0) == T_OK);
    CHECK(finalised == 1 && finalised_object == 0xE1);

    /* Revocation is resumable: with a budget it makes bounded progress. */
    CHECK(cap_install(&a, 0, CAP_ENDPOINT, CAP_R_ALL, 0xE2) == T_OK);
    for (cap_slot_t i = 1; i < 8; i++)
        CHECK(cap_derive(&a, i - 1, i, CAP_R_ALL, 0) == T_OK);  /* chain of 7 */
    CHECK(cap_revoke(&a, 0, 3) == T_EAGAIN);
    CHECK(cap_lookup(&a, 4, CAP_ENDPOINT, 0, &c) == T_OK);
    CHECK(cap_lookup(&a, 5, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(cdt_consistent(&a));
    CHECK(cap_revoke(&a, 0, 3) == T_EAGAIN);
    CHECK(cap_revoke(&a, 0, 3) == T_OK);
    CHECK(cap_lookup(&a, 1, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&a, 0, CAP_ENDPOINT, 0, &c) == T_OK);
    CHECK(finalised == 1);

    /* Destroying a cspace must leave no CDT link pointing into it. */
    CHECK(cap_derive_to(&a, 0, &b, 1, CAP_R_ALL, 0) == T_OK);
    CHECK(cap_derive_to(&b, 1, &a, 1, CAP_R_ALL, 0) == T_OK);  /* a0 > b1 > a1 */
    cspace_destroy(&b);
    CHECK(links_within(&a, cdt_store_a, cdt_store_a + ARRAY_SIZE(cdt_store_a)));
    CHECK(cdt_store_a[1].depth == 1 && cdt_store_a[1].prev == &cdt_store_a[0]);
    CHECK(cdt_consistent(&a));
    CHECK(finalised == 1);

    cspace_destroy(&a);
    CHECK(finalised == 2 && finalised_object == 0xE2);
    for (size_t i = 0; i < ARRAY_SIZE(cdt_store_a); i++)
        CHECK(cdt_store_a[i].type == CAP_NULL && cdt_store_b[i].type == CAP_NULL);

    cap_set_finaliser(CAP_ENDPOINT, NULL);
}

/* ---- Untyped and retype (docs/adr/0006-untyped-retype.md) ---- */

static struct cap ut_store[96];
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
    return untyped_boot_frames() + pmm_free_count() == pmm_total_count();
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
    CHECK(frames + pmm_free_count() == pmm_total_count()); /* conservation */
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
    CHECK(cap_retype(&uts, 0, CAP_THREAD, 0, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 1, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 16, &uts, 1, 1) == T_EINVAL); /* same size */
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 17, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_UNTYPED, 11, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 0, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_CNODE, 13, &uts, 1, 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, 0) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 1, RETYPE_MAX_COUNT + 1) == T_EINVAL);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 0xFFFFFFFFu, 2) == T_ERANGE);
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 95, 2) == T_ERANGE);
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
static struct { uint64_t base; uint64_t size; int parent; } objs[96];

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
        CHECK(cap_revoke(&uts, 0, 0) == T_OK);
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
    CHECK(!cap_has_children(&ut_store[1]));
    CHECK(ut_store[1].next == NULL || ut_store[1].next < cn_lo || ut_store[1].next >= cn_hi);
    CHECK(cdt_consistent(&uts));

    ut_teardown();
    CHECK(frames_conserved());
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

    /* Remove every executable mapping; now RW is allowed... */
    CHECK(vspace_unmap(&uts, 10) == T_OK);
    CHECK(vspace_map(&uts, 1, 13, ua(VA0 + 0x2000), RW) == T_EPERM); /* 12 still RX */
    CHECK(vspace_unmap(&uts, 12) == T_OK);
    CHECK(vspace_map(&uts, 1, 13, ua(VA0 + 0x2000), RW) == T_OK);

    /* ...and RW first blocks RX, in the other order too. */
    CHECK(vspace_map(&uts, 1, 10, ua(VA0), RX) == T_EPERM);
    CHECK(vspace_map(&uts, 5, 12, ua(VA0 + 0x1000), RX) == T_EPERM);
    CHECK(vspace_map(&uts, 5, 12, ua(VA0 + 0x1000), RO) == T_OK);

    /* Deleting a writable alias's capability lifts the block. */
    CHECK(cap_delete(&uts, 13) == T_OK);
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
     * tables, and its tables become installable elsewhere. */
    CHECK(vspace_activate(&uts, 1) == T_OK);
    CHECK(cap_delete(&uts, 1) == T_OK);
    CHECK(cr3_is_kernel(master));
    CHECK(ut_store[2].map_state == 0 && ut_store[2].map_entry == NULL);
    CHECK(ut_store[3].map_state == 0 && ut_store[3].map_entry == NULL); /* V15 */
    CHECK(cap_retype(&uts, 0, CAP_ADDRESS_SPACE, 0, &uts, 1, 1) == T_OK);
    CHECK(vspace_install(&uts, 1, 2, ua(VA0), 1) == T_OK);
    CHECK(vspace_audit(&uts, 1));


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

    /* V15: an uninstalled table carries no mappings into another address
     * space. Populate AS 5's tables, pull its PDPT out, and install it in
     * AS 1 instead: nothing that was mapped may become visible there. */
    CHECK(cap_retype(&uts, 0, CAP_FRAME, 0, &uts, 40, 2) == T_OK);
    CHECK(vspace_map(&uts, 5, 40, ua(VA0 + 0x10000), RW) == T_OK);
    CHECK(vspace_map(&uts, 5, 41, ua(VA0 + 0x11000), RO) == T_OK);
    CHECK(vspace_uninstall(&uts, 6) == T_OK);              /* AS 5's PDPT */
    CHECK(ut_store[40].map_state == 0 && ut_store[41].map_state == 0);
    CHECK(ut_store[7].map_state == 0 && ut_store[8].map_state == 0);
    CHECK(cap_delete(&uts, 2) == T_OK);                    /* free AS 1's slot */
    CHECK(vspace_install(&uts, 1, 6, ua(VA0), 1) == T_OK);
    CHECK(hw_flags(&uts, 1, VA0 + 0x10000) == 0 && hw_flags(&uts, 1, VA0 + 0x11000) == 0);
    CHECK(vspace_audit(&uts, 1) && vspace_audit(&uts, 5));

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
    { "vspace_perms", test_vspace_perms },
    { "vspace_alias", test_vspace_alias },
    { "vspace_deputy", test_vspace_deputy },
    { "vspace_teardown", test_vspace_teardown },
    { "w^x", test_wx },
    { "stack_guards", test_stack_guards },
    { "percpu_fpu", test_percpu_fpu },
    { "cpu_hardening", test_cpu_hardening },
};

__noreturn void selftest_run(void)
{
    kprintf("selftest: running %zu tests\n", ARRAY_SIZE(tests));

    for (size_t i = 0; i < ARRAY_SIZE(tests); i++) {
        unsigned before = failures;
        tests[i].fn();
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
