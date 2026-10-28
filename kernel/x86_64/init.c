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
#include <FS.h>
#include "init.h"

static void fat_runtime_smoke_test(void) {
    if (!disktable || !volumes)
        return;

    FS fs = disktable[0].parts[0].fs;
    if (fs != fat16 && fs != fat32)
        return;

    char root_path[] = "|A/";
    DIR *root = opendir(root_path);
    if (!root) {
        terminal_write("FAT TEST ROOT FAILED\n");
        diagnostics_log("FAT test: root directory open failed.");
        return;
    }

    bool directory_seen = false;
    bool directory_passed = false;
    bool file_seen = false;
    bool file_passed = false;
    dirent *entry;

    while ((entry = readdir(root)) != 0) {
        bool dot_entry = entry->name[0] == '.' && (entry->name[1] == '\0' || (entry->name[1] == '.' && entry->name[2] == '\0'));

        if (entry->type == DIRENT_DIR && !directory_seen && !dot_entry) {
            u32 name_length = 0;
            while (entry->name[name_length] && name_length < FILENAME_LENGTH - 4)
                ++name_length;
            if (name_length != 0 && entry->name[name_length] == '\0') {
                char path[FILENAME_LENGTH];
                memcpy(path, root_path, 3);
                memcpy(path + 3, entry->name, name_length);
                path[3 + name_length] = '\0';
                directory_seen = true;
                DIR *child = opendir(path);
                if (child) {
                    readdir(child);
                    directory_passed = closedir(child) == 0;
                }
            }
        }

        if (entry->type == DIRENT_FILE && entry->size != 0 && !file_seen) {
            u32 name_length = 0;
            while (entry->name[name_length] && name_length < FILENAME_LENGTH - 4)
                ++name_length;
            if (name_length != 0 && entry->name[name_length] == '\0') {
                char path[FILENAME_LENGTH];
                memcpy(path, root_path, 3);
                memcpy(path + 3, entry->name, name_length);
                path[3 + name_length] = '\0';
                file_seen = true;
                FILE *fp = fopen(path, 0);
                if (fp) {
                    u8 first = 0;
                    u8 last = 0;
                    bool first_read = fread(&first, 1, fp) == 1;
                    bool last_read = fseek(fp, -1, SEEK_END) == 0 && fread(&last, 1, fp) == 1;
                    file_passed = first_read && last_read;
                    if (fclose(fp) != 0)
                        file_passed = false;
                }
            }
        }
    }

    bool root_closed = closedir(root) == 0;
    if (root_closed) {
        terminal_write("FAT ROOT DIRECTORY OK\n");
        diagnostics_log("FAT test: root directory opened, enumerated and closed.");
    } else {
        terminal_write("FAT ROOT DIRECTORY FAILED\n");
        diagnostics_log("FAT test: root directory close failed.");
    }

    if (directory_seen && directory_passed) {
        terminal_write("FAT NESTED DIRECTORY OK\n");
        diagnostics_log("FAT test: nested directory opened and closed.");
    } else if (directory_seen) {
        terminal_write("FAT NESTED DIRECTORY FAILED\n");
        diagnostics_log("FAT test: nested directory open or close failed.");
    } else {
        terminal_write("FAT NESTED DIRECTORY SKIPPED\n");
        diagnostics_log("FAT test: no root directory entry was available for nested-directory testing.");
    }

    if (file_seen && file_passed) {
        terminal_write("FAT FILE READ AND SEEK OK\n");
        diagnostics_log("FAT test: file open, first-byte read, end-relative seek, last-byte read and close passed.");
    } else if (file_seen) {
        terminal_write("FAT FILE READ AND SEEK FAILED\n");
        diagnostics_log("FAT test: file open, read, seek or close failed.");
    } else {
        terminal_write("FAT FILE READ SKIPPED\n");
        diagnostics_log("FAT test: no non-empty file was available in the root directory.");
    }
}

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
        diagnostics_log("Boot checkpoint: terminal initialized.");
        diagnostics_log("Boot: framebuffer console initialized.");

        terminal_write("ANGOS KERNEL STARTED\n");
        terminal_write("PHYSICAL MEMORY INITIALIZING\n");

        phys_memory_init(loader_data);
        diagnostics_log("Boot checkpoint: physical memory initialized.");
        diagnostics_log("Boot: initializing physical memory.");
        terminal_write("PHYSICAL MEMORY OK\n");

        init_gdt();
        diagnostics_log("Boot checkpoint: GDT initialized.");
        diagnostics_log("Boot: GDT initialized.");
        terminal_write("GDT OK\n");

        init_idt();
        diagnostics_log("Boot checkpoint: IDT initialized.");
        diagnostics_log("Boot: IDT initialized.");
        terminal_write("IDT OK\n");

        if (!init_memory()) {
                diagnostics_log("Boot failure: paging or heap initialization failed.");
                terminal_write("PAGING FAILED\n");
                diagnostics_panic("Paging initialization failed (software)", 0, 0, false, 0, false, 0);
        }
        diagnostics_log("Boot checkpoint: paging and heap initialized.");
        diagnostics_log("Boot: paging initialization returned.");
        terminal_write("PAGING OK\n");

        keyboard_init();
        diagnostics_log("Boot checkpoint: keyboard initialized.");
        diagnostics_log("Boot: keyboard initialized.");

        init_timer();
        diagnostics_log("Boot checkpoint: timer initialized.");
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
        diagnostics_log("Boot checkpoint: disk and filesystem initialization finished.");
        fat_runtime_smoke_test();
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
        diagnostics_log("Boot checkpoint: kernel initialization complete.");

        __asm__ volatile("sti");
}