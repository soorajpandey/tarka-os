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

#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/paging.h"
#include "../arch/x86_64/qemu.h"

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
    paddr_t a = pmm_alloc();
    paddr_t b = pmm_alloc();

    CHECK(a != 0 && b != 0 && a != b);
    CHECK(a % PAGE_SIZE == 0 && b % PAGE_SIZE == 0);
    CHECK(pmm_free_count() == before - 2);

    /* Fresh frames never contain stale data. */
    const uint8_t *q = phys_to_virt(b);
    bool fresh = true;
    for (size_t i = 0; i < PAGE_SIZE; i++)
        fresh &= q[i] == 0;
    CHECK(fresh);

    /* Freed frames are scrubbed. */
    memset(phys_to_virt(a), 0xAA, PAGE_SIZE);
    CHECK(pmm_free(a) == T_OK);
    const uint8_t *p = phys_to_virt(a);
    bool zeroed = true;
    for (size_t i = 0; i < PAGE_SIZE; i++)
        zeroed &= p[i] == 0;
    CHECK(zeroed);

    CHECK(pmm_free(a) == T_EDOUBLEFREE);
    CHECK(pmm_free(b + 1) == T_EINVAL);
    CHECK(pmm_free(0) == T_EINVAL);
    CHECK(pmm_free(~0ull & ~(PAGE_SIZE - 1)) == T_EINVAL);
    /* Frames the allocator never owned (legacy VGA hole, always reserved
     * on PC) cannot be pushed into the free pool. */
    CHECK(pmm_free(0xA0000) == T_EINVAL);
    CHECK(pmm_free(b) == T_OK);
    CHECK(pmm_free_count() == before);

    /* Leak check: allocate and release many frames, nothing may be lost. */
    paddr_t frames[64];
    for (size_t i = 0; i < ARRAY_SIZE(frames); i++)
        frames[i] = pmm_alloc();
    for (size_t i = 0; i < ARRAY_SIZE(frames); i++)
        CHECK(frames[i] != 0 && pmm_free(frames[i]) == T_OK);
    CHECK(pmm_free_count() == before);
}

static void test_caps(void)
{
    struct cap storage[8];
    struct cspace cs;
    const struct cap *c;

    cspace_init(&cs, storage, ARRAY_SIZE(storage));

    CHECK(cap_install(&cs, 0, CAP_FRAME, CAP_R_READ | CAP_R_WRITE | CAP_R_GRANT, 0x1000) == T_OK);
    CHECK(cap_install(&cs, 0, CAP_FRAME, CAP_R_READ, 0x2000) == T_EBUSY);
    CHECK(cap_install(&cs, 8, CAP_FRAME, CAP_R_READ, 0) == T_ERANGE);
    CHECK(cap_install(&cs, 1, CAP_NULL, CAP_R_READ, 0) == T_EINVAL);
    CHECK(cap_install(&cs, 1, CAP_FRAME, 1u << 31, 0) == T_EINVAL);

    /* Lookup enforces type and rights. */
    CHECK(cap_lookup(&cs, 0, CAP_FRAME, CAP_R_WRITE, &c) == T_OK && c->object == 0x1000);
    CHECK(cap_lookup(&cs, 0, CAP_ENDPOINT, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 0, CAP_FRAME, CAP_R_EXEC, &c) == T_EPERM);
    CHECK(cap_lookup(&cs, 5, CAP_FRAME, 0, &c) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 0xFFFFFFFFu, CAP_FRAME, 0, &c) == T_ERANGE);

    /* Derivation can only narrow rights, never widen them. */
    CHECK(cap_derive(&cs, 0, 1, CAP_R_READ | CAP_R_EXEC, 0) == T_OK);
    CHECK(cap_lookup(&cs, 1, CAP_FRAME, CAP_R_READ, &c) == T_OK);
    CHECK(cap_lookup(&cs, 1, CAP_FRAME, CAP_R_EXEC, &c) == T_EPERM);
    CHECK(cap_lookup(&cs, 1, CAP_FRAME, CAP_R_WRITE, &c) == T_EPERM);

    /* Without GRANT a capability cannot be copied onward. */
    CHECK(cap_derive(&cs, 1, 2, CAP_R_ALL, 0) == T_EPERM);
    CHECK(cap_derive(&cs, 0, 1, CAP_R_READ, 0) == T_EBUSY);

    /* Badges are write-once. */
    CHECK(cap_derive(&cs, 0, 3, CAP_R_ALL, 0xB1) == T_OK);
    CHECK(cap_lookup(&cs, 3, CAP_FRAME, 0, &c) == T_OK && c->badge == 0xB1);
    CHECK(cap_derive(&cs, 3, 4, CAP_R_ALL, 0xB2) == T_EPERM);
    CHECK(cap_derive(&cs, 3, 4, CAP_R_READ, 0) == T_OK);
    CHECK(cap_lookup(&cs, 4, CAP_FRAME, 0, &c) == T_OK && c->badge == 0xB1);

    CHECK(cap_delete(&cs, 1) == T_OK);
    CHECK(cap_delete(&cs, 1) == T_EBADCAP);
    CHECK(cap_lookup(&cs, 1, CAP_FRAME, 0, &c) == T_EBADCAP);

    /* cap_identify exposes type, rights and badge only. */
    struct cap_info info;
    memset(&info, 0xEE, sizeof(info));
    CHECK(cap_identify(&cs, 3, &info) == T_OK);
    CHECK(info.type == CAP_FRAME && info.rights == (CAP_R_READ | CAP_R_WRITE | CAP_R_GRANT) &&
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

extern const char __text_start[], __text_end[];
extern const char __rodata_start[], __rodata_end[];
extern const char __data_start[], __data_end[];

extern char boot_stack_guard[], df_stack_guard[], nmi_stack_guard[];

static bool is_stack_guard(vaddr_t va)
{
    return va == (vaddr_t)boot_stack_guard || va == (vaddr_t)df_stack_guard ||
           va == (vaddr_t)nmi_stack_guard;
}

/* Every page in [start, end) must have exactly the expected W and NX bits;
 * the only unmapped pages allowed are the stack guards. */
static bool range_has(const char *start, const char *end, bool want_w, bool want_nx)
{
    for (vaddr_t va = ALIGN_DOWN((vaddr_t)start, PAGE_SIZE); va < (vaddr_t)end; va += PAGE_SIZE) {
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
    extern char boot_stack_top[];
    uint64_t f;
    char probe;

    CHECK(paging_query((vaddr_t)boot_stack_guard, &f) == -1);
    CHECK(paging_query((vaddr_t)df_stack_guard, &f) == -1);
    CHECK(paging_query((vaddr_t)nmi_stack_guard, &f) == -1);
    /* We are running on the guarded boot stack. */
    CHECK(&probe > boot_stack_guard + PAGE_SIZE && &probe < boot_stack_top);
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
    { "w^x", test_wx },
    { "stack_guards", test_stack_guards },
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

    kprintf("selftest: %u checks, %u failures\n", checks, failures);
    if (failures) {
        kprintf("SELFTEST FAILED\n");
        qemu_exit(QEMU_EXIT_FAIL);
    }
    kprintf("SELFTEST PASSED\n");
    qemu_exit(QEMU_EXIT_PASS);
}

#endif
