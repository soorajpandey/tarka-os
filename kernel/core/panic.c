#include <tarka/kernel.h>

#include "../arch/x86_64/cpu.h"

#ifdef TARKA_SELFTEST
#include "../arch/x86_64/qemu.h"
#endif

static volatile int panicking;

__noreturn void panic(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    __asm__ volatile("cli");

    /* A panic inside panic (e.g. a fault while formatting) must not
     * recurse forever. */
    if (panicking++)
        cpu_halt_forever();

    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kprintf("\n*** KERNEL PANIC: %s\n", buf);

#ifdef TARKA_SELFTEST
    qemu_exit(QEMU_EXIT_FAIL);
#endif
    cpu_halt_forever();
}
