#include "idt.h"

#include "cpu.h"
#include "gdt.h"

#include <tarka/kernel.h>
#include <tarka/selftest.h>
#include <tarka/uaccess.h>

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

extern const uint64_t isr_table[256];

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

static void set_gate(int vec, uint64_t handler, uint8_t ist, unsigned dpl)
{
    idt[vec] = (struct idt_entry){
        .off_lo = handler & 0xFFFF,
        .sel = GDT_KCODE,
        .ist = ist,
        .type_attr = (uint8_t)(0x8E | (dpl << 5)), /* present, 64-bit interrupt gate */
        .off_mid = (handler >> 16) & 0xFFFF,
        .off_hi = handler >> 32,
    };
}

void idt_init(void)
{
    /* Interrupt gates (IF cleared on entry) for every vector. DPL 0, so
     * `int n` from user mode raises #GP, except #BP: `int3` is how user
     * code (debuggers) asks for a breakpoint, and it must arrive as #BP. */
    for (int i = 0; i < 256; i++)
        set_gate(i, isr_table[i], 0, 0);
    set_gate(3, isr_table[3], 0, 3);
    set_gate(2, isr_table[2], IST_NMI, 0);
    set_gate(8, isr_table[8], IST_DOUBLE_FAULT, 0);
    set_gate(18, isr_table[18], IST_MC, 0);

    struct idtr idtr = { .limit = sizeof(idt) - 1, .base = (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}

static __noreturn void fatal(const struct trap_frame *tf, const char *where)
{
    const char *name = tf->vector < 32 ? exc_names[tf->vector] : "interrupt";

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
    panic("unhandled exception %lu in %s: %s (error %#lx)", tf->vector, where, name, tf->error);
}

/* Entries that interrupted kernel code (ring 0). The kernel runs with
 * interrupts disabled, so only stray interrupt vectors are tolerated;
 * every exception is a kernel bug (T30). */
void isr_dispatch(struct trap_frame *tf)
{
    if (tf->vector >= 32)
        return; /* spurious: nothing is configured to raise it */
    if (tf->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        /* T35: the one recoverable kernel-mode fault. IRETQ restores the
         * interrupted RFLAGS; the landing pad then clears AC. */
        if (copy_fault_recoverable(tf->rip, cr2)) {
#ifdef TARKA_SELFTEST
            entry_obs.copy_faults++;
#endif
            tf->rip = copy_fault_landing();
            return;
        }
    }
    fatal(tf, "kernel mode");
}

/* NMI, #DF and #MC, from any context, on their own IST stacks (T32). */
void paranoid_dispatch(struct trap_frame *tf);
void paranoid_dispatch(struct trap_frame *tf)
{
    if (tf->vector == 2) {
#ifdef TARKA_SELFTEST
        uint64_t rflags;
        __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
        entry_obs.nmis++;
        entry_obs.nmi_last_rflags = rflags;
        entry_obs.nmi_last_from_user = (tf->cs & 3) == 3;
#endif
        return;
    }
    fatal(tf, tf->vector == 8 ? "double fault" : "machine check");
}
