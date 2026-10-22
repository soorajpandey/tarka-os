#include <tarka/kernel.h>

/*
 * Stack canary for -fstack-protector-strong with
 * -mstack-protector-guard=global. The initial value is a placeholder; it
 * is replaced with a random value by stack_protector_init() before any
 * protected function can return.
 */
uintptr_t __stack_chk_guard = 0x595e9fbd94fda766ull;

__no_canary void stack_protector_init(uint64_t seed)
{
    /* Keep the low byte zero so string overflows (strcpy-style) cannot
     * reproduce the canary: they stop at the NUL. */
    __stack_chk_guard = (uintptr_t)(seed & ~0xFFull);
    if (__stack_chk_guard == 0)
        __stack_chk_guard = 0xdeadbeefcafe0000ull;
}

__noreturn __no_canary void __stack_chk_fail(void)
{
    panic("stack smashing detected");
}
