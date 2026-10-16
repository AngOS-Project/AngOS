#ifndef PAGING_H
#define PAGING_H

#include <types.h>
#include <memory.h>
#include <globals.h>

#define FLAG_PRESENT (1ULL << 0)
#define FLAG_RW      (1ULL << 1)
#define FLAG_US      (1ULL << 2)
#define FLAG_PS      (1ULL << 7)

#define FLAGS        (FLAG_PRESENT | FLAG_RW | FLAG_US)
#define ADDR_MASK    0x000FFFFFFFFFF000ULL

bool map_page(void *phys, void *virt);
bool map_2mb_page(void *phys, void *virt);
void *find_physaddr(void *virt);
bool unmap_page(void *virt);
bool init_paging(void);

extern void load_pml4(struct table *pml4);

#endif
