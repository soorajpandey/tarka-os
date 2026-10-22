#ifndef TARKA_X86_64_IDT_H
#define TARKA_X86_64_IDT_H

#include <tarka/types.h>

/* Must match the push order in isr.S. */
struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

void idt_init(void);
void isr_dispatch(struct trap_frame *tf);

#endif
