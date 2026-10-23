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
        if (c->slot_flags != 0 || c->pending_link != 0)
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

/* T33: deleting nested CNodes uses constant kernel stack. */
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
    CHECK(!cap_has_children(&ut_store[0])); /* all 1000 drained */
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
    /* Draining A pushes B, then C (C's link -> B), then revoking U2
     * deletes C[0]. */
    CHECK(cap_delete(&uts, 1) == T_OK);
    CHECK(!cap_has_children(&ut_store[0]));
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

#define PROGS(X) X(hello) X(fresh) X(fpu_a) X(fpu_b) X(preserve) X(ac) X(segbase) X(badsys) \
    X(f_ud) X(f_bp) X(f_de) X(f_pf_read) X(f_pf_write) X(f_pf_exec) X(f_gp_hlt) X(f_gp_io) \
    X(f_gp_int) X(f_gp_noncanon) X(f_ss) X(f_xm) X(f_ac) X(sysret_edge)
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
        cap_retype(&uts, 0, CAP_CNODE, 2, &uts, b + T_CS, 1) != T_OK ||
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
