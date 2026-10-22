#include "paging.h"

#include "cpu.h"

#include <tarka/kernel.h>

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* Find the leaf entry mapping `va`; *level_out is 0 for a 4 KiB page. */
static uint64_t *walk(vaddr_t va, int *level_out)
{
    uint64_t *table = phys_to_virt(read_cr3() & PTE_ADDR_MASK);

    for (int level = 3; level >= 0; level--) {
        uint64_t *e = &table[(va >> (12 + 9 * level)) & 0x1FF];

        if (!(*e & PTE_P))
            return NULL;
        if (level == 0 || (level <= 2 && (*e & PTE_PS))) {
            *level_out = level;
            return e;
        }
        table = phys_to_virt(*e & PTE_ADDR_MASK);
    }
    return NULL;
}

int paging_unmap_4k(vaddr_t va)
{
    int level;
    uint64_t *e = walk(va, &level);

    if (!e || level != 0)
        return -1;
    *e = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return 0;
}

extern char boot_stack_guard[], df_stack_guard[], nmi_stack_guard[];

void stack_guards_init(void)
{
    char *const guards[] = { boot_stack_guard, df_stack_guard, nmi_stack_guard };

    for (size_t i = 0; i < ARRAY_SIZE(guards); i++) {
        if (paging_unmap_4k((vaddr_t)guards[i]) != 0)
            panic("cannot unmap stack guard page %p", guards[i]);
    }
}

int paging_query(vaddr_t va, uint64_t *flags_out)
{
    uint64_t *table = phys_to_virt(read_cr3() & PTE_ADDR_MASK);
    uint64_t writable = PTE_W, user = PTE_U, nx = 0;

    for (int level = 3; level >= 0; level--) {
        unsigned idx = (va >> (12 + 9 * level)) & 0x1FF;
        uint64_t e = table[idx];

        if (!(e & PTE_P))
            return -1;

        /* Effective permissions are the intersection over all levels:
         * a page is writable/user only if every level allows it, and
         * no-execute if any level sets NX. */
        writable &= e;
        user &= e;
        nx |= e & PTE_NX;

        bool leaf = level == 0 || (level <= 2 && (e & PTE_PS));
        if (leaf) {
            *flags_out = PTE_P | writable | user | nx;
            return 0;
        }
        table = phys_to_virt(e & PTE_ADDR_MASK);
    }
    return -1;
}
