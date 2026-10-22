#ifndef TARKA_PMM_H
#define TARKA_PMM_H

#include <tarka/types.h>

struct limine_memmap_response;

/* Physical page frame allocator (bitmap). Frames are zeroed on free so
 * that no data leaks between owners. */
void pmm_init(struct limine_memmap_response *memmap);
__must_check paddr_t pmm_alloc(void);          /* 0 on failure */
__must_check int pmm_free(paddr_t frame);      /* T_EDOUBLEFREE, T_EINVAL */
uint64_t pmm_free_count(void);
uint64_t pmm_total_count(void);

#endif
