#include <stdlib.h>
#include <string.h>
#include <globals.h>
#include "paging.h"

struct table pml4;

#define PAGE_2M   0x200000ULL
#define FIRST_4GB 0x100000000ULL

static struct table *new_table(void) {
        void *page = phys_page_alloc();

        if (!page)
                return 0;

        memset(page, 0, PAGE_SIZE);
        return (struct table *)page;
}

static struct table *get_pdpt(u64 pml4_idx) {
        if (!(pml4.entry[pml4_idx] & FLAG_PRESENT)) {
                struct table *pdpt = new_table();

                if (!pdpt)
                        return 0;

                pml4.entry[pml4_idx] =
                        ((u64)pdpt & ADDR_MASK) | FLAGS;
        }

        return (struct table *)(pml4.entry[pml4_idx] & ADDR_MASK);
}

static struct table *get_pd(struct table *pdpt, u64 pdpt_idx) {
        if (!(pdpt->entry[pdpt_idx] & FLAG_PRESENT)) {
                struct table *pd = new_table();

                if (!pd)
                        return 0;

                pdpt->entry[pdpt_idx] =
                        ((u64)pd & ADDR_MASK) | FLAGS;
        }

        return (struct table *)(pdpt->entry[pdpt_idx] & ADDR_MASK);
}

bool map_2mb_page(void *phys, void *virt) {
        u64 p = (u64)phys;
        u64 v = (u64)virt;

        if ((p & (PAGE_2M - 1)) || (v & (PAGE_2M - 1)))
                return false;

        unsigned pml4_idx = (v >> 39) & 0x1FF;
        unsigned pdpt_idx = (v >> 30) & 0x1FF;
        unsigned pd_idx   = (v >> 21) & 0x1FF;

        struct table *pdpt = get_pdpt(pml4_idx);
        if (!pdpt)
                return false;

        struct table *pd = get_pd(pdpt, pdpt_idx);
        if (!pd)
                return false;

        pd->entry[pd_idx] =
                (p & ADDR_MASK) | FLAGS | FLAG_PS;
        return true;
}

bool map_page(void *phys, void *virt) {
        u64 p = (u64)phys;
        u64 v = (u64)virt;

        if ((p & (PAGE_SIZE - 1)) || (v & (PAGE_SIZE - 1)))
                return false;

        unsigned pml4_idx = (v >> 39) & 0x1FF;
        unsigned pdpt_idx = (v >> 30) & 0x1FF;
        unsigned pd_idx   = (v >> 21) & 0x1FF;
        unsigned pt_idx   = (v >> 12) & 0x1FF;

        struct table *pdpt = get_pdpt(pml4_idx);
        if (!pdpt)
                return false;

        struct table *pd = get_pd(pdpt, pdpt_idx);
        if (!pd)
                return false;

        if (pd->entry[pd_idx] & FLAG_PS)
                return false;

        struct table *pt;

        if (!(pd->entry[pd_idx] & FLAG_PRESENT)) {
                pt = new_table();

                if (!pt)
                        return false;

                pd->entry[pd_idx] =
                        ((u64)pt & ADDR_MASK) | FLAGS;
        } else {
                pt = (struct table *)(pd->entry[pd_idx] & ADDR_MASK);
        }

        pt->entry[pt_idx] =
                (p & ADDR_MASK) | FLAGS;
        return true;
}

void *find_physaddr(void *virt) {
        u64 v = (u64)virt;

        unsigned pml4_idx = (v >> 39) & 0x1FF;
        unsigned pdpt_idx = (v >> 30) & 0x1FF;
        unsigned pd_idx   = (v >> 21) & 0x1FF;
        unsigned pt_idx   = (v >> 12) & 0x1FF;

        if (!(pml4.entry[pml4_idx] & FLAG_PRESENT))
                return 0;

        struct table *pdpt =
                (struct table *)(pml4.entry[pml4_idx] & ADDR_MASK);

        if (!(pdpt->entry[pdpt_idx] & FLAG_PRESENT))
                return 0;

        struct table *pd =
                (struct table *)(pdpt->entry[pdpt_idx] & ADDR_MASK);

        if (!(pd->entry[pd_idx] & FLAG_PRESENT))
                return 0;

        if (pd->entry[pd_idx] & FLAG_PS) {
                u64 base = pd->entry[pd_idx] & ADDR_MASK;

                return (void *)(base + (v & (PAGE_2M - 1)));
        }

        struct table *pt =
                (struct table *)(pd->entry[pd_idx] & ADDR_MASK);

        if (!(pt->entry[pt_idx] & FLAG_PRESENT))
                return 0;

        return (void *)(
                (pt->entry[pt_idx] & ADDR_MASK) +
                (v & (PAGE_SIZE - 1))
        );
}

bool unmap_page(void *virt) {
        u64 v = (u64)virt;
        unsigned pml4_idx = (v >> 39) & 0x1FF;
        unsigned pdpt_idx = (v >> 30) & 0x1FF;
        unsigned pd_idx = (v >> 21) & 0x1FF;
        unsigned pt_idx = (v >> 12) & 0x1FF;

        if (!(pml4.entry[pml4_idx] & FLAG_PRESENT))
                return false;

        struct table *pdpt = (struct table *)(pml4.entry[pml4_idx] & ADDR_MASK);
        if (!(pdpt->entry[pdpt_idx] & FLAG_PRESENT))
                return false;

        struct table *pd = (struct table *)(pdpt->entry[pdpt_idx] & ADDR_MASK);
        if (!(pd->entry[pd_idx] & FLAG_PRESENT) || (pd->entry[pd_idx] & FLAG_PS))
                return false;

        struct table *pt = (struct table *)(pd->entry[pd_idx] & ADDR_MASK);
        if (!(pt->entry[pt_idx] & FLAG_PRESENT))
                return false;

        pt->entry[pt_idx] &= ~FLAG_PRESENT;
        __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
        return true;
}

bool init_paging(void) {
        memset(&pml4, 0, sizeof(pml4));

        for (u64 addr = 0;
             addr < FIRST_4GB;
             addr += PAGE_2M) {
                if (!map_2mb_page((void *)addr, (void *)addr))
                        return false;
        }

        for (size_t offset = 0;
             offset < kernel_size;
             offset += PAGE_SIZE) {
                if (!map_page(
                        (void *)((u64)kernel_paddr + offset),
                        (void *)((u64)kernel_vaddr + offset)
                ))
                        return false;
        }

        if (fb_addr && fb_size) {
                void *fb_phys = fb_addr;
                void *fb_virt = (void *)0x8000000000ULL;

                for (size_t offset = 0;
                     offset < fb_size;
                     offset += PAGE_SIZE) {
                        if (!map_page(
                                (void *)((u64)fb_phys + offset),
                                (void *)((u64)fb_virt + offset)
                        ))
                                return false;
                }

                fb_addr = fb_virt;
        }

        void *pml4_phys = find_physaddr(&pml4);

        if (!pml4_phys)
                return false;

        load_pml4((struct table *)pml4_phys);
        return true;
}
