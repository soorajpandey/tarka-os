#ifndef TARKA_X86_64_QEMU_H
#define TARKA_X86_64_QEMU_H

#include "cpu.h"

/* QEMU `-device isa-debug-exit,iobase=0xf4,iosize=0x04` terminates the VM
 * with exit status (value << 1) | 1. Test builds only. */
#define QEMU_EXIT_PASS 0x10 /* status 33 */
#define QEMU_EXIT_FAIL 0x11 /* status 35 */

static inline __noreturn void qemu_exit(uint32_t code)
{
    outl(0xf4, code);
    cpu_halt_forever();
}

#endif
