#include <types.h>
#include <diagnostics.h>
#include <memory.h>
#include <string.h>
#include <globals.h>
#include "memory.h"
#include <syscall.h>
#include <tosaithe.h>
#ifndef CEILING
#define CEILING(a, b) (((a) + (b) - 1) / (b))
#endif


static void memory_merge_free_spaces(int heap) {
        bool merged;
        do {
                merged = false;
                for (int i = 0; i < FREE_SPACES && !merged; ++i) {
                        if (freespace[heap][i].size == 0)
                                continue;
                        u64 i_start = (u64)freespace[heap][i].addr;
                        u64 i_end = i_start + freespace[heap][i].size;
                        for (int j = i + 1; j < FREE_SPACES; ++j) {
                                if (freespace[heap][j].size == 0)
                                        continue;
                                u64 j_start = (u64)freespace[heap][j].addr;
                                u64 j_end = j_start + freespace[heap][j].size;
                                if (i_end == j_start) {
                                        freespace[heap][i].size += freespace[heap][j].size;
                                        freespace[heap][j].addr = 0;
                                        freespace[heap][j].size = 0;
                                        merged = true;
                                        break;
                                }
                                if (j_end == i_start) {
                                        freespace[heap][i].addr = freespace[heap][j].addr;
                                        freespace[heap][i].size += freespace[heap][j].size;
                                        freespace[heap][j].addr = 0;
                                        freespace[heap][j].size = 0;
                                        merged = true;
                                        break;
                                }
                        }
                }
        } while (merged);
}

static bool memory_add_free_space(int heap, u64 address, size_t size) {
        int empty = -1;
        bool attached = false;
        for (int i = 0; i < FREE_SPACES; ++i) {
                if (freespace[heap][i].size == 0) {
                        if (empty < 0)
                                empty = i;
                        continue;
                }
                u64 start = (u64)freespace[heap][i].addr;
                u64 end = start + freespace[heap][i].size;
                if (end == address) {
                        freespace[heap][i].size += size;
                        attached = true;
                        break;
                }
                if (address + size == start) {
                        freespace[heap][i].addr = (void *)address;
                        freespace[heap][i].size += size;
                        attached = true;
                        break;
                }
        }
        if (!attached) {
                if (empty < 0) {
                        memory_merge_free_spaces(heap);
                        for (int i = 0; i < FREE_SPACES; ++i) {
                                if (freespace[heap][i].size == 0) {
                                        empty = i;
                                        break;
                                }
                        }
                }
                if (empty < 0)
                        return false;
                freespace[heap][empty].addr = (void *)address;
                freespace[heap][empty].size = size;
        }
        memory_merge_free_spaces(heap);
        return true;
}

/*
	Allocate virtual memory from a heap
*/
void *malloc_heap(size_t size, int heap) {
        if (heap < 0 || heap >= HEAPS || size == 0)
                return 0;

        int allocation = -1;
        for (int i = 0; i < ALLOCATIONS; ++i) {
                if (allocations[heap][i].size == 0) {
                        allocation = i;
                        break;
                }
        }
        if (allocation < 0) {
                diagnostics_log("Memory: allocation table is full.");
                return 0;
        }

        int space = -1;
        for (int i = 0; i < FREE_SPACES; ++i) {
                if (freespace[heap][i].size >= size) {
                        space = i;
                        break;
                }
        }
        if (space < 0)
                return 0;

        void *addr = freespace[heap][space].addr;
        freespace[heap][space].addr = (void *)((u64)addr + size);
        freespace[heap][space].size -= size;
        allocations[heap][allocation].addr = addr;
        allocations[heap][allocation].size = size;

        u64 first_page = (u64)addr & ~(u64)(PAGE_SIZE - 1);
        u64 end_page = ((u64)addr + size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        for (u64 page_addr = first_page; page_addr < end_page; page_addr += PAGE_SIZE) {
                void *virt = (void *)page_addr;
                if (find_physaddr(virt))
                        continue;
                void *phys = phys_page_alloc();
                if (!phys) {
                        diagnostics_log("Memory: no physical page was available for a heap mapping.");
                        free(addr);
                        return 0;
                }
                map_page(phys, virt);
                if (!find_physaddr(virt)) {
                        phys_page_free(phys);
                        diagnostics_log("Memory: virtual page mapping failed during heap allocation.");
                        free(addr);
                        return 0;
                }
        }
        return addr;
}

/*
	Allocate memory from the kernel heap
*/
void *malloc(size_t size) {
	return malloc_heap(size, 0);
}

/*
	Frees memory allocated from malloc
*/
void free(void *addr) {
        if (!addr)
                return;

        u64 address = (u64)addr;
        int heap = -1;
        if (address >= (u64)kernel_heap && address < (u64)kernel_stack)
                heap = 0;
        else if (address >= (u64)userspace_heap && address < (u64)userspace_stack)
                heap = 1;
        else
                return;

        int allocation = -1;
        size_t size = 0;
        for (int i = 0; i < ALLOCATIONS; ++i) {
                if (allocations[heap][i].size != 0 && allocations[heap][i].addr == addr) {
                        allocation = i;
                        size = allocations[heap][i].size;
                        break;
                }
        }
        if (allocation < 0)
                return;

        if (!memory_add_free_space(heap, address, size)) {
                diagnostics_log("Memory: free-space table is full; allocation remains tracked.");
                return;
        }

        allocations[heap][allocation].addr = 0;
        allocations[heap][allocation].size = 0;

        u64 first_page = address & ~(u64)(PAGE_SIZE - 1);
        u64 end_page = (address + size + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        for (u64 page_addr = first_page; page_addr < end_page; page_addr += PAGE_SIZE) {
                bool used = false;
                u64 page_end = page_addr + PAGE_SIZE;
                for (int i = 0; i < ALLOCATIONS; ++i) {
                        if (allocations[heap][i].size == 0)
                                continue;
                        u64 other_start = (u64)allocations[heap][i].addr;
                        u64 other_end = other_start + allocations[heap][i].size;
                        if (other_start < page_end && other_end > page_addr) {
                                used = true;
                                break;
                        }
                }
                if (used)
                        continue;
                void *phys = find_physaddr((void *)page_addr);
                if (!phys)
                        continue;
                if (unmap_page((void *)page_addr))
                        phys_page_free((void *)((u64)phys & ~(u64)(PAGE_SIZE - 1)));
        }
}


#define MAX_PHYS_REGIONS 512

typedef struct {
        u64 base;
        u64 pages;
} phys_region_t;

static phys_region_t managed_regions[MAX_PHYS_REGIONS];
static phys_region_t free_regions[MAX_PHYS_REGIONS];

static size_t managed_region_count;
static size_t free_region_count;

static u64 phys_total_pages;
static u64 phys_free_pages_count;

static void phys_sort_and_merge(phys_region_t *regions, size_t *count) {
        for (size_t i = 1; i < *count; ++i) {
                phys_region_t value = regions[i];
                size_t j = i;

                while (j > 0 && regions[j - 1].base > value.base) {
                        regions[j] = regions[j - 1];
                        --j;
                }

                regions[j] = value;
        }

        size_t out = 0;

        for (size_t i = 0; i < *count; ++i) {
                if (regions[i].pages == 0)
                        continue;

                if (out != 0) {
                        u64 end =
                                regions[out - 1].base +
                                regions[out - 1].pages * PAGE_SIZE;

                        if (end == regions[i].base) {
                                regions[out - 1].pages += regions[i].pages;
                                continue;
                        }
                }

                regions[out++] = regions[i];
        }

        *count = out;
}

static bool phys_managed(u64 addr) {
        for (size_t i = 0; i < managed_region_count; ++i) {
                u64 begin = managed_regions[i].base;
                u64 end =
                        begin +
                        managed_regions[i].pages * PAGE_SIZE;

                if (addr >= begin && addr < end)
                        return true;
        }

        return false;
}

static bool phys_free_already(u64 addr) {
        for (size_t i = 0; i < free_region_count; ++i) {
                u64 begin = free_regions[i].base;
                u64 end =
                        begin +
                        free_regions[i].pages * PAGE_SIZE;

                if (addr >= begin && addr < end)
                        return true;
        }

        return false;
}

static bool phys_insert_free_page(u64 addr) {
        size_t pos = 0;

        while (pos < free_region_count &&
               free_regions[pos].base < addr)
                ++pos;

        if (pos > 0) {
                phys_region_t *prev = &free_regions[pos - 1];
                u64 prev_end =
                        prev->base + prev->pages * PAGE_SIZE;

                if (prev_end == addr) {
                        ++prev->pages;

                        if (pos < free_region_count &&
                            addr + PAGE_SIZE == free_regions[pos].base) {
                                prev->pages += free_regions[pos].pages;

                                for (size_t i = pos;
                                     i + 1 < free_region_count;
                                     ++i) {
                                        free_regions[i] = free_regions[i + 1];
                                }

                                --free_region_count;
                        }

                        return true;
                }
        }

        if (pos < free_region_count &&
            addr + PAGE_SIZE == free_regions[pos].base) {
                free_regions[pos].base = addr;
                ++free_regions[pos].pages;
                return true;
        }

        if (free_region_count >= MAX_PHYS_REGIONS)
                return false;

        for (size_t i = free_region_count; i > pos; --i)
                free_regions[i] = free_regions[i - 1];

        free_regions[pos].base = addr;
        free_regions[pos].pages = 1;
        ++free_region_count;

        return true;
}

void phys_memory_init(const tosaithe_loader_data *loader_data) {
        managed_region_count = 0;
        free_region_count = 0;
        phys_total_pages = 0;
        phys_free_pages_count = 0;

        if (!loader_data || !loader_data->memmap)
                return;

        for (u32 i = 0; i < loader_data->memmap_entries; ++i) {
                const tsbp_mmap_entry *entry = &loader_data->memmap[i];

                if (entry->type != 0)
                        continue;

                u64 begin = (u64)entry->base;
                u64 end = begin + (u64)entry->length;

                begin = (begin + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
                end &= ~(u64)(PAGE_SIZE - 1);

                if (end <= begin)
                        continue;

                if (begin == 0) {
                        if (end <= PAGE_SIZE)
                                continue;

                        begin = PAGE_SIZE;
                }

                u64 pages = (end - begin) / PAGE_SIZE;

                if (managed_region_count >= MAX_PHYS_REGIONS)
                        break;

                managed_regions[managed_region_count].base = begin;
                managed_regions[managed_region_count].pages = pages;
                ++managed_region_count;
        }

        phys_sort_and_merge(managed_regions, &managed_region_count);

        for (size_t i = 0; i < managed_region_count; ++i) {
                free_regions[free_region_count++] = managed_regions[i];
                phys_total_pages += managed_regions[i].pages;
        }

        free_region_count = managed_region_count;
        phys_free_pages_count = phys_total_pages;
}

void *phys_page_alloc(void) {
        for (size_t i = 0; i < free_region_count; ++i) {
                phys_region_t *region = &free_regions[i];

                if (region->pages == 0)
                        continue;

                u64 addr = region->base;

                region->base += PAGE_SIZE;
                --region->pages;

                if (region->pages == 0) {
                        for (size_t j = i;
                             j + 1 < free_region_count;
                             ++j) {
                                free_regions[j] = free_regions[j + 1];
                        }

                        --free_region_count;
                }

                --phys_free_pages_count;
                return (void *)addr;
        }

        return 0;
}

void phys_page_free(void *addr) {
        u64 page = (u64)addr;

        if (page == 0 || (page & (PAGE_SIZE - 1)) != 0)
                return;

        if (!phys_managed(page))
                return;

        if (phys_free_already(page))
                return;

        if (!phys_insert_free_page(page))
                return;

        ++phys_free_pages_count;
}

u64 phys_memory_total_pages(void) {
        return phys_total_pages;
}

u64 phys_memory_free_pages(void) {
        return phys_free_pages_count;
}


/*
 * Combines contiguous free spaces into one entry
*/
void refresh_freespace(void) {
        for (int heap = 0; heap < HEAPS; ++heap)
                memory_merge_free_spaces(heap);
}

void free_pages(const void *virt, size_t pages) {
	unsigned int pml4_idx;
	unsigned int pdpt_idx;
	unsigned int pd_idx;
	unsigned int pt_idx;

	while (pages) {
		pml4_idx =	((u64) virt >> 39) & 0x1FF;
		pdpt_idx =	((u64) virt >> 30) & 0x1FF;
		pd_idx =	((u64) virt >> 21) & 0x1FF;
		pt_idx =	((u64) virt >> 12) & 0x1FF;

		struct table *pdpt = ((void *) pml4.entry[pml4_idx]);
		struct table *pd = ((void *) pdpt->entry[pdpt_idx]);
		struct table *pt = ((void *) pd->entry[pd_idx]);

		pt->entry[pt_idx] &= ~(1 << 0);

		virt += PAGE_SIZE;
		--pages;
	}
}

void init_malloc() {
	freespace[0][0].addr = kernel_heap;
	freespace[0][0].size = kernel_stack - kernel_heap;
	freespace[1][0].addr = userspace_heap;
	freespace[1][0].size = userspace_stack - userspace_heap;
}

void init_paging();

void init_memory() {
	init_paging();
	init_malloc();
}
