#ifndef MEMORY_H
#define MEMORY_H
#include <types.h>
#include <tosaithe.h>
#include <tosaithe.h>

#define PAGE_SIZE 4096

typedef struct {
	void *addr;
	size_t blocks;
} memory_t;

typedef struct {
	u32 type;
	u32 pad;
	void *physaddr;
	void *virtaddr;
	u64 pages;
	u64 attr;
} memmap_t;

typedef struct {
	void *addr;
	size_t blocks;
} section;

bool init_memory(void);
bool map_page(void *phys, void *virt);
bool unmap_page(void *virt);
void *find_physaddr(void *virt);
void *phys_page_alloc();
void phys_page_free(void *addr);

void phys_memory_init(const tosaithe_loader_data *loader_data);
u64 phys_memory_total_pages(void);
u64 phys_memory_free_pages(void);

#endif
