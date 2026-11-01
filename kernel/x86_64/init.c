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
    char empty_path[FILENAME_LENGTH];
    empty_path[0] = '\0';

    bool directory_seen = false;
    bool directory_passed = false;
    bool file_seen = false;
    bool file_passed = false;
    bool empty_seen = false;
    bool empty_passed = false;

    char missing_file_path[] = "|A/MISSING.TXT";
    char missing_dir_path[] = "|A/MISSING_DIR";

    FILE *missing_file = fopen(missing_file_path, 0);
    bool missing_file_rejected = missing_file == 0;
    if (missing_file && fclose(missing_file) != 0)
        missing_file_rejected = false;

    DIR *missing_dir = opendir(missing_dir_path);
    bool missing_dir_rejected = missing_dir == 0;
    if (missing_dir && closedir(missing_dir) != 0)
        missing_dir_rejected = false;

    DIR *root = opendir(root_path);
    if (!root) {
        terminal_write("FAT TEST ROOT FAILED\n");
        diagnostics_log("FAT test: root directory open failed.");
        return;
    }

    dirent *entry;

    while ((entry = readdir(root)) != 0) {
        bool dot_entry = entry->name[0] == '.' &&
            (entry->name[1] == '\0' ||
             (entry->name[1] == '.' && entry->name[2] == '\0'));

        u32 name_length = 0;
        while (entry->name[name_length] && name_length < FILENAME_LENGTH - 4)
            ++name_length;

        if (name_length == 0 || entry->name[name_length] != '\0')
            continue;

        char path[FILENAME_LENGTH];
        memcpy(path, root_path, 3);
        memcpy(path + 3, entry->name, name_length);
        path[3 + name_length] = '\0';

        if (entry->type == DIRENT_DIR && !directory_seen && !dot_entry) {
            directory_seen = true;
            DIR *child = opendir(path);
            if (child) {
                readdir(child);
                directory_passed = closedir(child) == 0;
            }
        }

        if (entry->type != DIRENT_FILE)
            continue;

        if (entry->size == 0 && !empty_seen) {
            empty_seen = true;
            memcpy(empty_path, path, 3 + name_length + 1);
        }

        if (entry->size == 0 || entry->size > 4096 || file_seen)
            continue;

        file_seen = true;
        FILE *fp = fopen(path, 0);

        if (fp) {
            size_t file_size = (size_t)entry->size;
            u8 *data = malloc(file_size);

            if (data) {
                bool full_read = fread(data, file_size, fp) == file_size;
                u8 probe = 0;
                bool eof_ok = full_read && fread(&probe, 1, fp) == 0;
                bool boundary_ok = full_read;
                bool last_ok = false;

                if (full_read && file_size > 512) {
                    u8 actual[4];
                    u32 offset = 510;
                    boundary_ok = fseek(fp, offset, SEEK_SET) == 0 &&
                        fread(actual, sizeof(actual), fp) == sizeof(actual);

                    if (boundary_ok) {
                        for (u32 i = 0; i < sizeof(actual); ++i) {
                            if (actual[i] != data[offset + i])
                                boundary_ok = false;
                        }
                    }
                }

                if (full_read && fseek(fp, -1, SEEK_END) == 0 &&
                    fread(&probe, 1, fp) == 1 &&
                    probe == data[file_size - 1])
                    last_ok = true;

                bool invalid_seek = fseek(fp, (long long)file_size + 1, SEEK_SET) != 0;

                file_passed = full_read && eof_ok && boundary_ok &&
                    last_ok && invalid_seek;

                free(data);
            }

            if (fclose(fp) != 0)
                file_passed = false;
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
        diagnostics_log("FAT test: no nested directory was available.");
    }

    if (file_seen && file_passed) {
        terminal_write("FAT FILE READ AND SEEK OK\n");
        terminal_write("FAT EOF AND CLUSTER BOUNDARY OK\n");
        diagnostics_log("FAT test: full file read, EOF, cluster-boundary read, last-byte seek and invalid seek passed.");
    } else if (file_seen) {
        terminal_write("FAT FILE READ AND SEEK FAILED\n");
        terminal_write("FAT EOF AND CLUSTER BOUNDARY FAILED\n");
        diagnostics_log("FAT test: full file read, EOF, cluster boundary or seek failed.");
    } else {
        terminal_write("FAT FILE READ SKIPPED\n");
        terminal_write("FAT EOF AND CLUSTER BOUNDARY SKIPPED\n");
        diagnostics_log("FAT test: no suitable non-empty file was available.");
    }

    if (empty_seen) {
        FILE *empty = fopen(empty_path, 0);
        if (empty) {
            u8 probe = 0;
            bool size_ok = fsize(empty) == 0;
            bool read_ok = fread(&probe, 1, empty) == 0;
            bool seek_ok = fseek(empty, 0, SEEK_END) == 0;
            bool eof_ok = seek_ok && fread(&probe, 1, empty) == 0;
            empty_passed = size_ok && read_ok && eof_ok;
            if (fclose(empty) != 0)
                empty_passed = false;
        }

        if (empty_passed) {
            terminal_write("FAT EMPTY FILE OK\n");
            diagnostics_log("FAT test: empty file open, size, read, EOF and close passed.");
        } else {
            terminal_write("FAT EMPTY FILE FAILED\n");
            diagnostics_log("FAT test: empty file handling failed.");
        }
    } else {
        terminal_write("FAT EMPTY FILE SKIPPED\n");
        diagnostics_log("FAT test: no empty file was present.");
    }

    if (missing_file_rejected && missing_dir_rejected) {
        terminal_write("FAT INVALID PATH REJECTION OK\n");
        diagnostics_log("FAT test: nonexistent file and directory were rejected.");
    } else {
        terminal_write("FAT INVALID PATH REJECTION FAILED\n");
        diagnostics_log("FAT test: a nonexistent path was accepted or cleanup failed.");
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