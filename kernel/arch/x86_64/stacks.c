#include "stacks.h"

extern char boot_stack_guard[], boot_stack[], boot_stack_top[];
extern char kstack_cpu0_guard[], kstack_cpu0[], kstack_cpu0_top[];
extern char df_stack_guard[], df_stack[], df_stack_top[];
extern char nmi_stack_guard[], nmi_stack[], nmi_stack_top[];
extern char mc_stack_guard[], mc_stack[], mc_stack_top[];

#define KSTACK(n, b) { #n, n##_guard, n, n##_top, (b) }

const struct kstack kstacks[] = {
    KSTACK(boot_stack, 16384),
    KSTACK(kstack_cpu0, 8192),
    KSTACK(df_stack, 8192),
    KSTACK(nmi_stack, 8192),
    KSTACK(mc_stack, 8192),
};
const size_t kstack_count = sizeof(kstacks) / sizeof(kstacks[0]);

void stacks_paint(void)
{
    char *rsp;

    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    for (size_t i = 0; i < kstack_count; i++) {
        const struct kstack *s = &kstacks[i];
        char *end = s->top;

        /* The stack we are running on: paint only well below RSP. */
        if (rsp > s->base && rsp <= s->top)
            end = rsp - 512;
        for (uint64_t *p = (uint64_t *)s->base; (char *)p < end; p++)
            *p = STACK_PAINT;
    }
}

size_t stack_high_water(const struct kstack *s)
{
    const uint64_t *p = (const uint64_t *)s->base;

    while ((const char *)p < s->top && *p == STACK_PAINT)
        p++;
    return (size_t)(s->top - (const char *)p);
}
