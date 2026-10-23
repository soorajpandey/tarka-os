#ifndef TARKA_X86_64_CPU_H
#define TARKA_X86_64_CPU_H

#include <tarka/types.h>

#define CR0_MP   (1ull << 1)
#define CR0_EM   (1ull << 2)
#define CR0_TS   (1ull << 3)
#define CR0_NE   (1ull << 5)
#define CR0_WP   (1ull << 16)
#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)
#define CR4_OSXSAVE    (1ull << 18)
#define CR4_UMIP (1ull << 11)
#define CR4_SMEP (1ull << 20)
#define CR4_SMAP (1ull << 21)

#define MSR_EFER  0xC0000080u
#define EFER_NXE  (1ull << 11)

/* Hardening features the kernel managed to enable at boot. */
struct cpu_security {
    bool nx;
    bool wp;
    bool smep;
    bool smap;
    bool umip;
    bool rdrand;
};

extern struct cpu_security cpu_sec;

void cpu_init(void);

/* Extended (FPU/vector) state, ADR 0008 T6/T7. */
#define XSAVE_AREA_MAX 4096  /* one page per thread */
#define MXCSR_DEFAULT  0x1F80u

extern uint64_t xcr0;        /* components enabled at boot */
extern uint32_t xsave_size;  /* bytes XSAVE writes for them */

void fpu_init(void);

/* Prepare `area` (XSAVE_AREA_MAX bytes, 64-byte aligned) so that XRSTOR
 * loads the architectural initial state (T7). */
void fpu_init_area(void *area);
void fpu_save(void *area);
void fpu_restore(const void *area);
bool cpu_rdrand64(uint64_t *out);

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

#define DEFINE_CR(n)                                                    \
    static inline uint64_t read_cr##n(void)                             \
    {                                                                   \
        uint64_t v;                                                     \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v));               \
        return v;                                                       \
    }                                                                   \
    static inline void write_cr##n(uint64_t v)                          \
    {                                                                   \
        __asm__ volatile("mov %0, %%cr" #n : : "r"(v) : "memory");      \
    }
DEFINE_CR(0)
DEFINE_CR(3)
DEFINE_CR(4)
#undef DEFINE_CR

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline __noreturn void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

#endif
