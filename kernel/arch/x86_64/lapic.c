#include "lapic.h"

#include "cpu.h"
#include "paging.h"

#include <tarka/kernel.h>

/*
 * Local APIC. x2APIC (MSRs) when the CPU has it, otherwise xAPIC through
 * its MMIO page, which the kernel maps uncached, non-executable and
 * supervisor-only at boot (before the first AddressSpace copies the kernel
 * half, ADR 0007 V4).
 */

#define MSR_APIC_BASE   0x1Bu
#define APIC_BASE_EN    (1ull << 11)
#define APIC_BASE_EXTD  (1ull << 10)
#define APIC_BASE_ADDR  0xFFFFFF000ull

/* Register numbers: xAPIC MMIO offset = reg << 4, x2APIC MSR = 0x800 + reg. */
#define R_EOI      0x0B
#define R_SVR      0x0F
#define R_ICR_LO   0x30
#define R_ICR_HI   0x31
#define R_LVT_TMR  0x32
#define R_TMR_INIT 0x38
#define R_TMR_CUR  0x39
#define R_TMR_DIV  0x3E

#define ICR_FIXED    0u
#define ICR_NMI      (4u << 8)
#define ICR_SELF     (1u << 18)
#define LVT_MASKED   (1u << 16)

#define CPUID1_ECX_X2APIC (1u << 21)
#define CPUID1_EDX_APIC   (1u << 9)

static bool present, x2;
static volatile uint32_t *mmio;

static void wr(unsigned reg, uint64_t v)
{
    if (x2)
        wrmsr(0x800 + reg, v);
    else
        mmio[reg << 2] = (uint32_t)v;
}

static uint32_t rd(unsigned reg)
{
    if (x2)
        return (uint32_t)rdmsr(0x800 + reg);
    return mmio[reg << 2];
}

static void icr(uint32_t lo)
{
    if (x2) {
        wrmsr(0x800 + R_ICR_LO, lo);
    } else {
        mmio[R_ICR_HI << 2] = 0;
        mmio[R_ICR_LO << 2] = lo;   /* writing the low half sends */
    }
}

bool lapic_init(void)
{
    uint32_t a, b, c, d;

    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & CPUID1_EDX_APIC))
        return false;
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (c & CPUID1_ECX_X2APIC) {
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN | APIC_BASE_EXTD);
        x2 = true;
    } else {
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_EN);
        mmio = paging_map_mmio(pa(base & APIC_BASE_ADDR));
        if (!mmio)
            return false;
    }
    wr(R_SVR, (1u << 8) | VEC_SPURIOUS);   /* software enable */
    present = true;
    kprintf("lapic: %s enabled\n", x2 ? "x2APIC" : "xAPIC (MMIO)");
    return true;
}

bool lapic_present(void) { return present; }

void lapic_eoi(void)
{
    if (present)
        wr(R_EOI, 0);
}

void lapic_self_ipi(uint8_t vector)
{
    icr(ICR_FIXED | ICR_SELF | vector);
}

void lapic_self_nmi(void)
{
    icr(ICR_NMI | ICR_SELF);
}

volatile uint32_t *lapic_icr_lo_mmio(void)
{
    return x2 ? NULL : &mmio[R_ICR_LO << 2];
}

void lapic_timer_setup(void)
{
    KASSERT(present);
    wr(R_TMR_DIV, 0x3);                       /* divide by 16 */
    lapic_timer_stop();
}

void lapic_timer_arm(uint32_t count)
{
    KASSERT(present && count > 0);            /* 0 would mean "stopped" */
    wr(R_LVT_TMR, VEC_TIMER);                 /* unmasked, one-shot */
    wr(R_TMR_INIT, count);                    /* starts the count */
}

void lapic_timer_stop(void)
{
    KASSERT(present);
    wr(R_TMR_INIT, 0);                        /* stops it; the count reads 0 */
    wr(R_LVT_TMR, LVT_MASKED | VEC_TIMER);
}

uint32_t lapic_timer_count(void)
{
    KASSERT(present);
    return rd(R_TMR_CUR);
}

uint32_t lapic_timer_divide(void)
{
    KASSERT(present);
    return rd(R_TMR_DIV) & 0xB;   /* bits 0, 1 and 3 */
}
