#ifndef TARKA_KERNEL_H
#define TARKA_KERNEL_H

#include <stdarg.h>
#include <tarka/types.h>

/* lib/printf.c */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* core/panic.c */
__noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define KASSERT(cond)                                                       \
    do {                                                                    \
        if (unlikely(!(cond)))                                              \
            panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)

/* core/stack_protector.c */
void stack_protector_init(uint64_t seed);
__noreturn void __stack_chk_fail(void);

/* core/main.c: entered from arch/x86_64/entry.S */
__noreturn void kmain(void);

/* Higher-half direct map offset supplied by the bootloader. */
extern uint64_t hhdm_offset;

static inline void *phys_to_virt(paddr_t p) { return (void *)(pa_raw(p) + hhdm_offset); }
static inline paddr_t virt_to_phys_hhdm(const void *v) { return pa((uint64_t)v - hhdm_offset); }

#endif
