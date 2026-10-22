#include "idt.h"

#include "cpu.h"
#include "gdt.h"

#include <tarka/kernel.h>

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __packed;

struct idtr {
    uint16_t limit;
    uint64_t base;
} __packed;

static struct idt_entry idt[256];

extern const uint64_t isr_table[32];

static const char *const exc_names[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint",
    "#OF overflow", "#BR bound range", "#UD invalid opcode", "#NM device not available",
    "#DF double fault", "coprocessor overrun", "#TS invalid TSS", "#NP segment not present",
    "#SS stack fault", "#GP general protection", "#PF page fault", "reserved",
    "#MF x87 FP", "#AC alignment check", "#MC machine check", "#XM SIMD FP",
    "#VE virtualization", "#CP control protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "#HV hypervisor injection", "#VC VMM communication", "#SX security", "reserved",
};

static void set_gate(int vec, uint64_t handler, uint8_t ist)
{
    idt[vec] = (struct idt_entry){
        .off_lo = handler & 0xFFFF,
        .sel = GDT_KCODE,
        .ist = ist,
        .type_attr = 0x8E, /* present, DPL0, 64-bit interrupt gate */
        .off_mid = (handler >> 16) & 0xFFFF,
        .off_hi = handler >> 32,
    };
}

void idt_init(void)
{
    for (int i = 0; i < 32; i++)
        set_gate(i, isr_table[i], 0);
    set_gate(2, isr_table[2], IST_NMI);
    set_gate(8, isr_table[8], IST_DOUBLE_FAULT);

    struct idtr idtr = { .limit = sizeof(idt) - 1, .base = (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

void isr_dispatch(struct trap_frame *tf)
{
    const char *name = tf->vector < 32 ? exc_names[tf->vector] : "unknown";

    /* No exception is recoverable yet; every fault in kernel mode is a bug.
     * Once user space exists, user faults will be delivered to the faulting
     * task's fault-handler endpoint instead of panicking the kernel. */
    if (tf->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("page fault at %#lx err=%#lx (%s%s%s%s)\n", cr2, tf->error,
                tf->error & 1 ? "protection " : "not-present ",
                tf->error & 2 ? "write " : "read ",
                tf->error & 4 ? "user " : "kernel ",
                tf->error & 16 ? "ifetch" : "");
    }
    kprintf("rip=%#lx cs=%#lx rflags=%#lx rsp=%#lx ss=%#lx\n",
            tf->rip, tf->cs, tf->rflags, tf->rsp, tf->ss);
    kprintf("rax=%#lx rbx=%#lx rcx=%#lx rdx=%#lx\n", tf->rax, tf->rbx, tf->rcx, tf->rdx);
    kprintf("rsi=%#lx rdi=%#lx rbp=%#lx\n", tf->rsi, tf->rdi, tf->rbp);
    panic("unhandled exception %lu: %s (error %#lx)", tf->vector, name, tf->error);
}
