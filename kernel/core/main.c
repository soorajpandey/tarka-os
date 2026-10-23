#include <tarka/kernel.h>
#include <tarka/pmm.h>
#include <tarka/selftest.h>
#include <tarka/untyped.h>
#include <tarka/thread.h>
#include <tarka/vspace.h>

#include <limine.h>

#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/gdt.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/paging.h"
#include "../arch/x86_64/percpu.h"
#include "../arch/x86_64/serial.h"
#include "../arch/x86_64/stacks.h"

/* Limine boot protocol requests. They live in .data so the bootloader can
 * fill in the response pointers, bracketed by start/end markers. */
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;

static uint64_t boot_entropy(void)
{
    uint64_t seed;

    if (cpu_rdrand64(&seed))
        return seed;

    /* Weak fallback for CPUs without RDRAND. Good enough to defeat a fixed
     * canary, not good enough for cryptographic keys. */
    kprintf("warning: no RDRAND, stack canary seeded from TSC\n");
    seed = rdtsc();
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed ^ rdtsc();
}

/* kmain switches the stack canary, so it must not itself be protected:
 * its own epilogue check would see the old value. It never returns. */
__noreturn __no_canary void kmain(void)
{
    serial_init();
    kprintf("\nTarka OS booting\n");

    if (!LIMINE_BASE_REVISION_SUPPORTED)
        panic("bootloader does not support Limine base revision 3");
    if (!hhdm_req.response || !memmap_req.response)
        panic("bootloader did not answer required requests");

    hhdm_offset = hhdm_req.response->offset;

    gdt_init();
    idt_init();
    cpu_init();
    fpu_init();
    percpu_init();
    stack_guards_init();
    stacks_paint();
    stack_protector_init(boot_entropy());
    pmm_init(memmap_req.response);
    untyped_boot();
    vspace_boot();
    thread_boot();

#ifdef TARKA_SELFTEST
    selftest_run();
#endif

    kprintf("Tarka OS: kernel initialised, nothing else to do yet. Halting.\n");
    cpu_halt_forever();
}
