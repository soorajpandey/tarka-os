#include "cpu.h"

#include <tarka/kernel.h>

struct cpu_security cpu_sec;

/* CPUID feature bits */
#define CPUID1_ECX_RDRAND   (1u << 30)
#define CPUID1_ECX_XSAVE    (1u << 26)
#define CPUID1_EDX_FXSR     (1u << 24)
#define CPUID1_EDX_SSE2     (1u << 26)
#define CPUID7_EBX_SMEP     (1u << 7)
#define CPUID7_EBX_SMAP     (1u << 20)
#define CPUID7_ECX_UMIP     (1u << 2)
#define CPUID81_EDX_NX      (1u << 20)

/*
 * Turn on every protection the CPU supports before anything else runs:
 *  - CR0.WP:   the kernel itself cannot write to read-only pages.
 *  - EFER.NXE: honour the no-execute bit (W^X).
 *  - SMEP:     the kernel can never execute user-space memory.
 *  - SMAP:     the kernel can only touch user memory inside explicit
 *              copy_{from,to}_user windows (stac/clac).
 *  - UMIP:     user space cannot read GDT/IDT/TR locations (sgdt etc.),
 *              which would leak kernel addresses.
 */
void cpu_init(void)
{
    uint32_t a, b, c, d;
    uint32_t max_leaf, max_ext;

    cpuid(0, 0, &max_leaf, &b, &c, &d);
    cpuid(0x80000000u, 0, &max_ext, &b, &c, &d);

    /* AM: alignment checking for ring 3 when the user sets RFLAGS.AC, so
     * misalignment is a reportable user fault (#AC). It never applies in
     * ring 0, where AC is always clear (ADR 0008, T32). */
    write_cr0(read_cr0() | CR0_WP | CR0_AM);
    cpu_sec.wp = true;

    if (max_ext >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &d);
        if (d & CPUID81_EDX_NX) {
            wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
            cpu_sec.nx = true;
        }
    }

    cpuid(1, 0, &a, &b, &c, &d);
    cpu_sec.rdrand = (c & CPUID1_ECX_RDRAND) != 0;

    if (max_leaf >= 7) {
        uint64_t cr4 = read_cr4();

        cpuid(7, 0, &a, &b, &c, &d);
        if (b & CPUID7_EBX_SMEP) {
            cr4 |= CR4_SMEP;
            cpu_sec.smep = true;
        }
        if (b & CPUID7_EBX_SMAP) {
            cr4 |= CR4_SMAP;
            cpu_sec.smap = true;
        }
        if (c & CPUID7_ECX_UMIP) {
            cr4 |= CR4_UMIP;
            cpu_sec.umip = true;
        }
        write_cr4(cr4);
    }

    if (!cpu_sec.nx)
        panic("CPU lacks NX support; refusing to boot without W^X");
    /* The user boundary depends on SMEP and SMAP (K3, ADR 0008 T32), and
     * every entry stub executes clac, which needs SMAP. */
    if (!cpu_sec.smep || !cpu_sec.smap)
        panic("CPU lacks SMEP/SMAP; refusing to boot without user-memory isolation");

    kprintf("cpu: WP=%d NX=%d SMEP=%d SMAP=%d UMIP=%d RDRAND=%d\n",
            cpu_sec.wp, cpu_sec.nx, cpu_sec.smep, cpu_sec.smap, cpu_sec.umip,
            cpu_sec.rdrand);
}

bool cpu_rdrand64(uint64_t *out)
{
    if (!cpu_sec.rdrand)
        return false;

    /* RDRAND can transiently fail; Intel recommends up to 10 retries. */
    for (int i = 0; i < 10; i++) {
        uint8_t ok;
        uint64_t v;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return true;
        }
    }
    return false;
}

/* ---- extended state (ADR 0008, T6, T7) ---- */

#define XCR0_X87     (1ull << 0)
#define XCR0_SSE     (1ull << 1)
#define XCR0_AVX     (1ull << 2)
#define XCR0_AVX512  (7ull << 5)  /* opmask, ZMM_Hi256, Hi16_ZMM */

uint64_t xcr0;
uint32_t xsave_size;

static inline void xsetbv(uint32_t idx, uint64_t v)
{
    __asm__ volatile("xsetbv" : : "c"(idx), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

void fpu_init(void)
{
    uint32_t a, b, c, d;

    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & CPUID1_EDX_FXSR) || !(d & CPUID1_EDX_SSE2) || !(c & CPUID1_ECX_XSAVE))
        panic("CPU lacks FXSR/SSE2/XSAVE; refusing to boot without full context switching");

    /* FPU present and owned by whoever runs: no lazy switching (EM, TS
     * clear), native error reporting (NE). */
    write_cr0((read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_OSXSAVE);

    cpuid(0xD, 0, &a, &b, &c, &d);
    uint64_t supported = ((uint64_t)d << 32) | a;
    uint64_t want = supported & (XCR0_X87 | XCR0_SSE | XCR0_AVX | XCR0_AVX512);

    /* AVX-512 only as a whole, and only if the area still fits in a page. */
    if ((want & XCR0_AVX512) != XCR0_AVX512)
        want &= ~XCR0_AVX512;
    for (;;) {
        xsetbv(0, want);
        cpuid(0xD, 0, &a, &b, &c, &d);
        if (b <= XSAVE_AREA_MAX)
            break;
        if (want & XCR0_AVX512)
            want &= ~XCR0_AVX512;
        else if (want & XCR0_AVX)
            want &= ~XCR0_AVX;
        else
            panic("XSAVE area for x87+SSE exceeds %u bytes", XSAVE_AREA_MAX);
    }
    xcr0 = want;
    xsave_size = b;
    kprintf("fpu: xcr0=%#lx (x87 SSE%s%s), xsave area %u bytes, eager switching\n", xcr0,
            (xcr0 & XCR0_AVX) ? " AVX" : "", (xcr0 & XCR0_AVX512) ? " AVX-512" : "", xsave_size);
}

void fpu_init_area(void *area)
{
    uint8_t *p = area;

    for (size_t i = 0; i < XSAVE_AREA_MAX; i++)
        p[i] = 0;
    /* XSTATE_BV = 0 in the header (offset 512) makes XRSTOR load the
     * initial state of every component. MXCSR (offset 24) is loaded from
     * the legacy area regardless, so give it its architectural default
     * rather than 0 (which would unmask every SIMD exception). */
    *(uint32_t *)(p + 24) = MXCSR_DEFAULT;
}

void fpu_save(void *area)
{
    __asm__ volatile("xsave64 (%0)" : : "r"(area), "a"((uint32_t)xcr0),
                     "d"((uint32_t)(xcr0 >> 32)) : "memory");
}

void fpu_restore(const void *area)
{
    __asm__ volatile("xrstor64 (%0)" : : "r"(area), "a"((uint32_t)xcr0),
                     "d"((uint32_t)(xcr0 >> 32)) : "memory");
}
