#include "gdt.h"

#include <tarka/string.h>

/*
 * Selector layout is fixed by SYSCALL/SYSRET (STAR MSR): user data must sit
 * directly below user code.
 *   0x00 null | 0x08 kernel code | 0x10 kernel data
 *   0x18 user data | 0x20 user code | 0x28 TSS (16 bytes)
 */

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __packed;

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __packed;

/* Dedicated stacks (entry.S) for faults that may fire on a corrupt or
 * overflowed kernel stack. Running the handler on the faulting stack would
 * just fault again. */
extern char df_stack_top[], nmi_stack_top[];

static struct tss tss;
static uint64_t gdt[7];

static uint64_t seg(uint8_t access, uint8_t flags)
{
    return ((uint64_t)access << 40) | ((uint64_t)flags << 52) | 0x000F00000000FFFFull;
}

void gdt_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.ist[IST_DOUBLE_FAULT - 1] = (uint64_t)df_stack_top;
    tss.ist[IST_NMI - 1] = (uint64_t)nmi_stack_top;
    /* An iomap_base beyond the TSS limit means no I/O bitmap: ring 3 gets
     * no I/O port access by default. Port access will be granted only via
     * CAP_IOPORT capabilities. */
    tss.iomap_base = sizeof(tss);

    gdt[0] = 0;
    gdt[1] = seg(0x9A, 0xA); /* kernel code: present, ring0, exec/read, L */
    gdt[2] = seg(0x92, 0xC); /* kernel data */
    gdt[3] = seg(0xF2, 0xC); /* user data: ring3 */
    gdt[4] = seg(0xFA, 0xA); /* user code: ring3, L */

    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdtr gdtr = { .limit = sizeof(gdt) - 1, .base = (uint64_t)gdt };

    __asm__ volatile(
        "lgdt %0\n"
        "mov %1, %%ds\n"
        "mov %1, %%es\n"
        "mov %1, %%ss\n"
        "xor %%eax, %%eax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "pushq %2\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "ltr %w3\n"
        :
        : "m"(gdtr), "r"((uint64_t)GDT_KDATA), "i"(GDT_KCODE), "r"((uint64_t)GDT_TSS)
        : "rax", "memory");
}
