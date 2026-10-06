#include <stdlib.h>
#include <string.h>
#include <cpu/GDT.h>
#include <cpu/IO.h>
#include <cpu/IDT.h>
#include <cpu/PIT.h>
#include <memory.h>
#include <types.h>
#include <syscall.h>
#include <tosaithe.h>
#include <terminal.h>
#include <globals.h>
#include <keyboard.h>
#include <disk.h>
#include "init.h"

void init(tosaithe_loader_data *loader_data) {
        if (!loader_data || !loader_data->kern_map) {
                return;
        }

        fb_addr = loader_data->framebuffer_addr;
        fb_size = loader_data->framebuffer_size;
        fb_w = loader_data->framebuffer_width;
        fb_h = loader_data->framebuffer_height;
        fb_pixperline = loader_data->framebuffer_pitch;

        u64 lowest_vaddr = ~0ULL;
        u64 highest_vaddr = 0;
        u64 lowest_paddr = 0;

        for (u32 i = 0; i < loader_data->kern_map_entries; ++i) {
                const tsbp_kernel_mapping *mapping =
                        &loader_data->kern_map[i];

                if (mapping->base_virt < lowest_vaddr) {
                        lowest_vaddr = mapping->base_virt;
                        lowest_paddr = mapping->base_phys;
                }

                u64 end =
                        mapping->base_virt +
                        mapping->length;

                if (end > highest_vaddr)
                        highest_vaddr = end;
        }

        kernel_vaddr = (void *)lowest_vaddr;
        kernel_paddr = (void *)lowest_paddr;
        kernel_size = highest_vaddr - lowest_vaddr;

        terminal_init();

        terminal_write("ANGOS KERNEL STARTED\n");
        terminal_write("PHYSICAL MEMORY INITIALIZING\n");

        phys_memory_init(loader_data);
        terminal_write("PHYSICAL MEMORY OK\n");

        init_gdt();
        terminal_write("GDT OK\n");

        init_idt();
        terminal_write("IDT OK\n");

        init_memory();
        terminal_write("PAGING OK\n");

        keyboard_init();

        init_timer();
        terminal_write("PIT OK\n");

        void *test = malloc(8192);

        terminal_write("KERNEL HEAP FAILED\n");
        }

        terminal_write("DISK INITIALIZING\n");

        init_disk();

        if (disktable &&
            disktable[0].parts[0].fs != none) {

                if (disktable[0].parts[0].fs == fat12)
                        terminal_write("FAT12 DETECTED\n");
                else if (disktable[0].parts[0].fs == fat16)
                        terminal_write("FAT16 DETECTED\n");
                else if (disktable[0].parts[0].fs == fat32)
                        terminal_write("FAT32 DETECTED\n");

        } else {
                terminal_write("DISK FILESYSTEM FAILED\n");
        }

        terminal_write("ANGOS IS RUNNING\n");

        __asm__ volatile("sti");
}