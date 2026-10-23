#include "percpu.h"

#include "cpu.h"

#include <stddef.h>

_Static_assert(offsetof(struct percpu, self) == PERCPU_SELF, "percpu layout");
_Static_assert(offsetof(struct percpu, kstack_top) == PERCPU_KSTACK_TOP, "percpu layout");
_Static_assert(offsetof(struct percpu, user_rsp) == PERCPU_USER_RSP, "percpu layout");
_Static_assert(offsetof(struct percpu, current) == PERCPU_CURRENT, "percpu layout");

static struct percpu cpu0;

void percpu_init(void)
{
    extern char kstack_cpu0_top[];

    cpu0.self = &cpu0;
    cpu0.kstack_top = (uint64_t)kstack_cpu0_top;
    cpu0.cpu_id = 0;
    /* While in the kernel, GS_BASE is ours and KERNEL_GS_BASE holds the
     * user value; swapgs exchanges them on every user/kernel crossing. */
    wrmsr(MSR_GS_BASE, (uint64_t)&cpu0);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    wrmsr(MSR_FS_BASE, 0);
}
