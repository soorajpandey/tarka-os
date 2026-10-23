#ifndef TARKA_X86_64_STACKS_H
#define TARKA_X86_64_STACKS_H

#include <tarka/types.h>

/* Every kernel stack, in one table (ADR 0008, T33). Guard pages, stack
 * painting, high-water measurement and the W^X test all iterate it, so a
 * stack cannot be added without being guarded and measured. */
struct kstack {
    const char *name;
    char       *guard;   /* unmapped page directly below the stack */
    char       *base;    /* lowest usable byte */
    char       *top;     /* initial RSP */
    size_t      budget;  /* worst-case bytes allowed (half the stack) */
};

extern const struct kstack kstacks[];
extern const size_t kstack_count;

#define STACK_PAINT 0x5354414b5354414bull /* "KATSKATS" */

/* Fill every unused stack with STACK_PAINT. Must run before any of them is
 * used; for the boot stack, only the part below the current RSP. */
void stacks_paint(void);

/* Deepest use so far, in bytes, measured from the top. */
size_t stack_high_water(const struct kstack *s);

#endif
