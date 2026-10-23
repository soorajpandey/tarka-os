/*
 * Bitmap physical frame allocator. Two bitmaps, one bit per 4 KiB frame up
 * to the highest usable address:
 *   used:    set = allocated or reserved
 *   managed: set = frame came from usable RAM and belongs to the allocator
 * The second bitmap is what lets pmm_free() reject frames it never owned
 * (kernel image, ACPI tables, MMIO, the bitmaps themselves) instead of
 * releasing them into the free pool.
 */

#include <tarka/pmm.h>

#include <tarka/errno.h>
#include <tarka/kernel.h>
#include <tarka/string.h>

#include <limine.h>

static uint64_t *used;
static uint64_t *managed;
static uint64_t  nframes;     /* frames covered by the bitmaps */
static uint64_t  free_frames;
static uint64_t  kernel_frames;  /* allocated by pmm_alloc, not yet freed */
static uint64_t  total_frames;
static uint64_t  next_hint;   /* search start, in frames */

static inline bool test_bit(const uint64_t *bm, uint64_t f) { return bm[f / 64] & (1ull << (f % 64)); }
static inline void set_bit(uint64_t *bm, uint64_t f) { bm[f / 64] |= 1ull << (f % 64); }
static inline void clear_bit(uint64_t *bm, uint64_t f) { bm[f / 64] &= ~(1ull << (f % 64)); }

void pmm_init(struct limine_memmap_response *memmap)
{
    uint64_t top = 0;

    KASSERT(memmap != NULL);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        uint64_t end;

        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        if (add_overflow(e->base, e->length, &end))
            panic("pmm: memmap entry %lu overflows", i);
        if (end > top)
            top = end;
    }

    nframes = top / PAGE_SIZE;
    uint64_t one_bitmap = (nframes + 63) / 64 * sizeof(uint64_t);
    uint64_t bitmap_bytes = ALIGN_UP(2 * one_bitmap, PAGE_SIZE);
    uint64_t bitmap_phys = 0; /* raw: bootloader-provided, validated here */

    /* Place both bitmaps in the first usable region big enough for them. */
    for (uint64_t i = 0; i < memmap->entry_count && !bitmap_phys; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        uint64_t base = ALIGN_UP(e->base, PAGE_SIZE);

        if (e->type != LIMINE_MEMMAP_USABLE || base == 0)
            continue;
        if (e->base + e->length >= base + bitmap_bytes)
            bitmap_phys = base;
    }
    if (!bitmap_phys)
        panic("pmm: no region large enough for %lu-byte bitmaps", bitmap_bytes);

    used = phys_to_virt(pa(bitmap_phys));
    managed = used + one_bitmap / sizeof(uint64_t);

    /* Everything starts used and unmanaged; only usable RAM is released. */
    memset(used, 0xFF, one_bitmap);
    memset(managed, 0x00, one_bitmap);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t first = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t f = first; f < last; f++) {
            uint64_t p = f * PAGE_SIZE;
            if (p >= bitmap_phys && p < bitmap_phys + bitmap_bytes)
                continue;
            /* Never hand out frame 0: a physical address of 0 is our
             * allocation-failure sentinel. */
            if (f == 0)
                continue;
            set_bit(managed, f);
            clear_bit(used, f);
            free_frames++;
        }
    }
    total_frames = free_frames;

    /* TODO: reclaim LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE once nothing
     * references the bootloader's responses any more. */

    kprintf("pmm: %lu MiB usable (%lu frames), bitmaps %lu KiB\n",
            free_frames * PAGE_SIZE >> 20, free_frames, bitmap_bytes >> 10);
}

paddr_t pmm_alloc(void)
{
    for (uint64_t n = 0; n < nframes; n++) {
        uint64_t f = (next_hint + n) % nframes;
        if (!test_bit(used, f)) {
            set_bit(used, f);
            free_frames--;
            kernel_frames++;
            next_hint = f + 1;
            /* Never return garbage: frames that were usable at boot were
             * never scrubbed and may hold bootloader or firmware data. */
            memset(phys_to_virt(pa(f * PAGE_SIZE)), 0, PAGE_SIZE);
            return pa(f * PAGE_SIZE);
        }
    }
    return pa(0);
}

static bool range_free(uint64_t f, uint64_t n);

paddr_t pmm_alloc_contig(uint64_t n)
{
    if (n == 0 || n > free_frames)
        return pa(0);
    for (uint64_t f = 1; f + n <= nframes; f++) {
        if (!range_free(f, n))
            continue;
        for (uint64_t i = f; i < f + n; i++)
            set_bit(used, i);
        free_frames -= n;
        kernel_frames += n;
        memset(phys_to_virt(pa(f * PAGE_SIZE)), 0, n * PAGE_SIZE);
        return pa(f * PAGE_SIZE);
    }
    return pa(0);
}

int pmm_free(paddr_t frame)
{
    uint64_t p = pa_raw(frame);

    if (p % PAGE_SIZE || p == 0 || p / PAGE_SIZE >= nframes)
        return T_EINVAL;

    uint64_t f = p / PAGE_SIZE;
    if (!test_bit(managed, f))
        return T_EINVAL;
    if (!test_bit(used, f))
        return T_EDOUBLEFREE;

    /* Scrub on free as well, so stale data never lingers in the pool. */
    memset(phys_to_virt(frame), 0, PAGE_SIZE);
    clear_bit(used, f);
    free_frames++;
    kernel_frames--;
    return T_OK;
}

/* True if frames [f, f + n) are all managed and free. */
static bool range_free(uint64_t f, uint64_t n)
{
    if (f + n > nframes)
        return false;
    for (uint64_t i = f; i < f + n;) {
        /* Whole 64-frame words at a time where possible. */
        if (i % 64 == 0 && f + n - i >= 64) {
            if (used[i / 64] != 0 || managed[i / 64] != ~0ull)
                return false;
            i += 64;
            continue;
        }
        if (test_bit(used, i) || !test_bit(managed, i))
            return false;
        i++;
    }
    return true;
}

bool pmm_take_block(unsigned max_bits, uint64_t keep, paddr_t *base, unsigned *bits)
{
    for (unsigned b = max_bits; b >= 12; b--) {
        uint64_t n = 1ull << (b - 12);

        if (free_frames < keep || free_frames - keep < n)
            continue;
        for (uint64_t f = 0; f + n <= nframes; f += n) {
            if (!range_free(f, n))
                continue;
            for (uint64_t i = f; i < f + n; i++)
                set_bit(used, i);
            free_frames -= n;
            *base = pa(f * PAGE_SIZE);
            *bits = b;
            return true;
        }
    }
    return false;
}

bool pmm_frame_is_free(paddr_t frame)
{
    uint64_t f = pa_raw(frame) / PAGE_SIZE;
    return f < nframes && test_bit(managed, f) && !test_bit(used, f);
}

uint64_t pmm_free_count(void) { return free_frames; }
uint64_t pmm_kernel_count(void) { return kernel_frames; }
uint64_t pmm_total_count(void) { return total_frames; }
uint64_t pmm_frame_limit(void) { return nframes; }
