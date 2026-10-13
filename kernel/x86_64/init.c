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
#include <diagnostics.h>
#include "init.h"

void init(tosaithe_loader_data *loader_data) {
        diagnostics_init();
        diagnostics_log("Boot: kernel init entered.");
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
        diagnostics_log("Boot: framebuffer console initialized.");

        terminal_write("ANGOS KERNEL STARTED\n");
        terminal_write("PHYSICAL MEMORY INITIALIZING\n");

        phys_memory_init(loader_data);
        diagnostics_log("Boot: initializing physical memory.");
        terminal_write("PHYSICAL MEMORY OK\n");

        init_gdt();
        diagnostics_log("Boot: GDT initialized.");
        terminal_write("GDT OK\n");

        init_idt();
        diagnostics_log("Boot: IDT initialized.");
        terminal_write("IDT OK\n");

        init_memory();
        diagnostics_log("Boot: paging initialization returned.");
        terminal_write("PAGING OK\n");

        keyboard_init();
        diagnostics_log("Boot: keyboard initialized.");

        init_timer();
        diagnostics_log("Boot: timer initialized.");
        terminal_write("PIT OK\n");

        void *test = malloc(8192);

        if (test) {
                memset(test, 0xA5, 8192);
                terminal_write("KERNEL HEAP OK\n");
                free(test);
        } else {
                terminal_write("KERNEL HEAP FAILED\n");
        }

        terminal_write("DISK INITIALIZING\n");

        diagnostics_log("Boot: initializing disk/filesystem.");
        init_disk();
        diagnostics_log("Boot: disk initialization returned.");

        if (disktable &&
            disktable[0].parts[0].fs != none) {

                if (disktable[0].parts[0].fs == fat12) {
                        terminal_write("FAT12 DETECTED\n");
                } else if (disktable[0].parts[0].fs == fat16) {
                        terminal_write("FAT16 DETECTED\n");
                } else if (disktable[0].parts[0].fs == fat32) {
                        terminal_write("FAT32 DETECTED\n");
                }

        } else {
                terminal_write("DISK FILESYSTEM FAILED\n");
        }

        diagnostics_log("Boot: kernel initialization complete.");
        terminal_write("ANGOS IS RUNNING\n");

        __asm__ volatile("sti");
}