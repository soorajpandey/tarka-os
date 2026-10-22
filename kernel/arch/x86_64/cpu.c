#include "cpu.h"

#include <tarka/kernel.h>

struct cpu_security cpu_sec;

/* CPUID feature bits */
#define CPUID1_ECX_RDRAND   (1u << 30)
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

    write_cr0(read_cr0() | CR0_WP);
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
