#include "paging.h"

#include "cpu.h"
#include "stacks.h"

#include <tarka/kernel.h>
#include <tarka/pmm.h>

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* Find the leaf entry mapping `va`; *level_out is 0 for a 4 KiB page. */
static uint64_t *walk(uint64_t va, int *level_out)
{
    uint64_t *table = phys_to_virt(pa(read_cr3() & PTE_ADDR_MASK));

    for (int level = 3; level >= 0; level--) {
        uint64_t *e = &table[(va >> (12 + 9 * level)) & 0x1FF];

        if (!(*e & PTE_P))
            return NULL;
        if (level == 0 || (level <= 2 && (*e & PTE_PS))) {
            *level_out = level;
            return e;
        }
        table = phys_to_virt(pa(*e & PTE_ADDR_MASK));
    }
    return NULL;
}

int paging_unmap_4k(uint64_t va)
{
    int level;
    uint64_t *e = walk(va, &level);

    if (!e || level != 0)
        return -1;
    *e = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return 0;
}

#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)

void *paging_map_mmio(paddr_t p)
{
    uint64_t va = pa_raw(p) + hhdm_offset;
    uint64_t *table = phys_to_virt(pa(read_cr3() & PTE_ADDR_MASK));

    for (int level = 3; level > 0; level--) {
        uint64_t *e = &table[(va >> (12 + 9 * level)) & 0x1FF];

        if (!(*e & PTE_P)) {
            if (level == 3)
                return NULL; /* never add kernel-half PML4 entries (V4) */
            paddr_t t = pmm_alloc();   /* zeroed */
            if (pa_is_null(t))
                return NULL;
            *e = pa_raw(t) | PTE_P | PTE_W;
        } else if (*e & PTE_PS) {
            return NULL;     /* covered by a large page: wrong cache type */
        }
        table = phys_to_virt(pa(*e & PTE_ADDR_MASK));
    }
    uint64_t *leaf = &table[(va >> 12) & 0x1FF];
    if (*leaf & PTE_P)
        return NULL;
    *leaf = pa_raw(p) | PTE_P | PTE_W | PTE_PCD | PTE_PWT | PTE_NX;
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    return (void *)va;
}

void stack_guards_init(void)
{
    for (size_t i = 0; i < kstack_count; i++) {
        if (paging_unmap_4k((uint64_t)(uintptr_t)kstacks[i].guard) != 0)
            panic("cannot unmap guard page of %s", kstacks[i].name);
    }
}

int paging_query(uint64_t va, uint64_t *flags_out)
{
    uint64_t *table = phys_to_virt(pa(read_cr3() & PTE_ADDR_MASK));
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
        table = phys_to_virt(pa(e & PTE_ADDR_MASK));
    }
    return -1;
}
