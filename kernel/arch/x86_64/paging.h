#ifndef TARKA_X86_64_PAGING_H
#define TARKA_X86_64_PAGING_H

#include <tarka/types.h>

#define PTE_P  (1ull << 0)
#define PTE_W  (1ull << 1)
#define PTE_U  (1ull << 2)
#define PTE_PS (1ull << 7)
#define PTE_NX (1ull << 63)

/* Walk the active page tables and report the effective P/W/U/NX flags for
 * `va`. Returns 0 if mapped, -1 if not. Used to verify W^X at runtime. */
int paging_query(uint64_t va, uint64_t *flags_out);

/* Unmap one 4 KiB kernel page. Returns 0, or -1 if `va` is unmapped or
 * part of a large page. */
int paging_unmap_4k(uint64_t va);

/* Map one device page (uncached, NX, supervisor-only) at its HHDM address,
 * taking any missing table pages from the kernel allocator. Boot only:
 * must run before the first AddressSpace is created (ADR 0007, V4).
 * Returns NULL if the address is already covered by another mapping. */
void *paging_map_mmio(paddr_t p);

/* Unmap the guard page below every kernel stack (see entry.S). */
void stack_guards_init(void);

#endif
