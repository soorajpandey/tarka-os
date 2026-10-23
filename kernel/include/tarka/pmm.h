#ifndef TARKA_PMM_H
#define TARKA_PMM_H

#include <tarka/types.h>

struct limine_memmap_response;

/* Physical page frame allocator (bitmap). Frames are zeroed on free so
 * that no data leaks between owners. */
void pmm_init(struct limine_memmap_response *memmap);
__must_check paddr_t pmm_alloc(void);          /* null (pa_is_null) on failure */
__must_check int pmm_free(paddr_t frame);      /* T_EDOUBLEFREE, T_EINVAL */
uint64_t pmm_free_count(void);
/* Frames the kernel itself holds (pmm_alloc without pmm_free). */
uint64_t pmm_kernel_count(void);

/* Take the largest naturally aligned block of 2^bits bytes (12 <= bits <=
 * max_bits) whose frames are all free, provided at least `keep` frames
 * stay free afterwards. The frames are marked used and permanently leave
 * the allocator (they now belong to an Untyped, ADR 0006). Returns false
 * when no such block exists. */
__must_check bool pmm_take_block(unsigned max_bits, uint64_t keep, paddr_t *base, unsigned *bits);

/* True if `frame` belongs to the allocator and is currently free. */
bool pmm_frame_is_free(paddr_t frame);
uint64_t pmm_total_count(void);

#endif
