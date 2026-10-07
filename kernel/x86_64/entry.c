#include <keyboard.h>
#include <terminal.h>
#include <cpu/IO.h>
#include <disk.h>
#include <stdio.h>
#include "../fs/FAT.h"

static int strcmp_local(
    const char *s1,
    const char *s2
) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }

    return *(const unsigned char *)s1 -
           *(const unsigned char *)s2;
}

static void print_u64_decimal(u64 value) {
    char buf[21];
    int i = 20;

    buf[i] = '\0';

    if (value == 0) {
        terminal_write("0");
        return;
    }

    while (value > 0 && i > 0) {
        buf[--i] =
            '0' + (value % 10);
        value /= 10;
    }

    terminal_write(&buf[i]);
}

static char hex_digit(u8 value) {
    return value < 10
        ? ('0' + value)
        : ('A' + value - 10);
}

static void print_u8_hex(u8 value) {
    char buf[3];

    buf[0] =
        hex_digit((value >> 4) & 0x0F);

    buf[1] =
        hex_digit(value & 0x0F);

    buf[2] = '\0';

    terminal_write(buf);
}

static bool starts_with_local(
    const char *s,
    const char *prefix
) {
    while (*prefix) {
        if (*s != *prefix)
            return false;

        ++s;
        ++prefix;
    }

    return true;
}

static void shell_ls(
    const char *argument
) {
    char path[128];

    path[0] = '|';
    path[1] = 'A';
    path[2] = '/';

    u32 i = 0;

    while (argument[i] &&
           i < sizeof(path) - 4) {

        path[i + 3] =
            argument[i];

        ++i;
    }

    path[i + 3] = '\0';

    DIR *dir =
        opendir(path);

    if (!dir) {
        terminal_write(
            "ls: unable to open directory\n"
        );
        return;
    }

    dirent *entry;

    while ((entry = readdir(dir)) != 0) {
        terminal_write(entry->name);

        if (entry->type == DIRENT_DIR)
            terminal_write("/");

        terminal_write("\n");
    }

    closedir(dir);
}

static void shell_stat(
    const char *argument
) {
    if (!argument || argument[0] == '\0') {
        terminal_write(
            "stat: missing path\n"
        );
        return;
    }

    char path[128];

    path[0] = '|';
    path[1] = 'A';
    path[2] = '/';

    u32 i = 0;

    while (argument[i] &&
           i < sizeof(path) - 4) {

        path[i + 3] =
            argument[i];

        ++i;
    }

    path[i + 3] = '\0';

    terminal_write("Name: ");
    terminal_write(argument);
    terminal_write("\n");

    FILE *fp = fopen(path, 0);

    if (fp) {
        terminal_write(
            "Type: File\n"
        );

        terminal_write(
            "Size: "
        );

        print_u64_decimal(
            fp->size
        );

        terminal_write(
            " bytes\n"
        );

        fclose(fp);
        return;
    }

    DIR *dir = opendir(path);

    if (dir) {
        terminal_write(
            "Type: Directory\n"
        );

        closedir(dir);
        return;
    }

    terminal_write(
        "stat: path not found\n"
    );
}

void execute_command(const char *cmd) {
    if (strcmp_local(cmd, "help") == 0) {

        terminal_write(
            "AngOS Built-in Commands:\n"
        );

        terminal_write(
            "  help     - Show available commands\n"
        );

        terminal_write(
            "  cls      - Clear terminal screen\n"
        );

        terminal_write(
            "  ver      - Display OS kernel version\n"
        );

        terminal_write(
            "  disk     - Detect the primary disk\n"
        );

        terminal_write(
            "  diskread - Read sector 0\n"
        );

        terminal_write(
            "  diskinit - Initialize disk/filesystem table\n"
        );

        terminal_write(
            "  fstest   - Test the filesystem API\n"
        );

        terminal_write(
            "  ls       - List a directory\n"
        );

        terminal_write(
            "  cat      - Read a file\n"
        );

        terminal_write(
            "  fat      - Detect the FAT filesystem\n"
        );

        terminal_write(
            "  fatls    - Legacy FAT16 directory listing\n"
        );

        terminal_write(
            "  fatcat   - Legacy FAT16 file reader\n"
        );

        terminal_write(
            "  reboot   - Reboot system\n"
        );

    } else if (strcmp_local(cmd, "cls") == 0) {

        terminal_clear();

    } else if (strcmp_local(cmd, "ver") == 0) {

        terminal_write(
            "AngOS v0.1.0 (x86_64 Architecture)\n"
        );

    } else if (strcmp_local(cmd, "disk") == 0) {

        u64 blocks = 0;

        terminal_write(
            "=== Disk ===\n"
        );

        if (disk_probe(&blocks)) {

            terminal_write(
                "ATA primary master: DETECTED\n"
            );

            terminal_write(
                "Sectors: "
            );

            print_u64_decimal(blocks);

            terminal_write("\n");

        } else {

            terminal_write(
                "ATA primary master: NOT DETECTED\n"
            );
        }

    } else if (
        strcmp_local(cmd, "diskread") == 0
    ) {

        u8 sector[512];

        terminal_write(
            "=== Disk Read ===\n"
        );

        if (disk_read_sector(
                0,
                sector,
                0
            )) {

            terminal_write(
                "Sector 0: READ OK\n"
            );

            terminal_write(
                "Signature: 0x"
            );

            print_u8_hex(sector[510]);
            print_u8_hex(sector[511]);

            terminal_write("\n");

            if (sector[510] == 0x55 &&
                sector[511] == 0xAA) {

                terminal_write(
                    "Boot signature: PRESENT\n"
                );

            } else {

                terminal_write(
                    "Boot signature: NOT FOUND\n"
                );
            }

        } else {

            terminal_write(
                "Sector 0: READ FAILED\n"
            );
        }

    } else if (
        strcmp_local(cmd, "diskinit") == 0
    ) {

        init_disk();

        if (!disktable) {

            terminal_write(
                "Disk table initialization failed\n"
            );

        } else {

            terminal_write(
                "Disk table initialized\n"
            );

            if (disktable[0].parts[0].fs ==
                fat12) {

                terminal_write(
                    "Partition 0: FAT12\n"
                );

            } else if (
                disktable[0].parts[0].fs ==
                fat16
            ) {

                terminal_write(
                    "Partition 0: FAT16\n"
                );

            } else if (
                disktable[0].parts[0].fs ==
                fat32
            ) {

                terminal_write(
                    "Partition 0: FAT32\n"
                );

            } else {

                terminal_write(
                    "Partition 0: UNKNOWN\n"
                );
            }
        }

    } else if (
        strcmp_local(cmd, "fstest") == 0
    ) {

        FILE *fp =
            fopen(
                "|A/EFI/BOOT/BOOTX64.EFI",
                0
            );

        if (!fp) {

            terminal_write(
                "fopen: FAILED\n"
            );

        } else {

            u8 buffer[16];

            terminal_write(
                "fopen: OK\n"
            );

            terminal_write(
                "File size: "
            );

            print_u64_decimal(fp->size);

            terminal_write(
                " bytes\n"
            );

            size_t read =
                fread(
                    buffer,
                    16,
                    fp
                );

            terminal_write(
                "fread: "
            );

            print_u64_decimal(read);

            terminal_write(
                " bytes\n"
            );

            terminal_write(
                "Data: "
            );

            for (u32 i = 0;
                 i < read;
                 ++i) {

                print_u8_hex(buffer[i]);

                terminal_write(
                    " "
                );
            }

            terminal_write("\n");

            fclose(fp);
        }

    } else if (
        strcmp_local(cmd, "ls") == 0
    ) {

        shell_ls("");

    } else if (
        starts_with_local(cmd, "ls ")
    ) {

        shell_ls(cmd + 3);

    } else if (
        starts_with_local(cmd, "cat ")
    ) {

        const char *name =
            cmd + 4;

        char path[128];

        path[0] = '|';
        path[1] = 'A';
        path[2] = '/';

        u32 i = 0;

        while (name[i] &&
               i < sizeof(path) - 4) {

            path[i + 3] =
                name[i];

            ++i;
        }

        path[i + 3] = '\0';

        FILE *fp =
            fopen(path, 0);

        if (!fp) {

            terminal_write(
                "cat: file not found\n"
            );

        } else {

            u8 buffer[512];

            while (1) {

                size_t read =
                    fread(
                        buffer,
                        sizeof(buffer),
                        fp
                    );

                if (read == 0)
                    break;

                for (u32 j = 0;
                     j < read;
                     ++j) {

                    char c =
                        (char)buffer[j];

                    if (c >= 32 ||
                        c == '\n' ||
                        c == '\r' ||
                        c == '\t') {

                        char out[2];

                        out[0] = c;
                        out[1] = '\0';

                        terminal_write(
                            out
                        );
                    }
                }
            }

            terminal_write("\n");

            fclose(fp);
        }

    } else if (
        strcmp_local(cmd, "fat") == 0
    ) {

        int type =
            fat_detect(0);

        if (type == 12)
            terminal_write(
                "FAT12 filesystem detected\n"
            );
        else if (type == 16)
            terminal_write(
                "FAT16 filesystem detected\n"
            );
        else if (type == 32)
            terminal_write(
                "FAT32 filesystem detected\n"
            );
        else
            terminal_write(
                "FAT filesystem not detected\n"
            );

    } else if (
        strcmp_local(cmd, "fatls") == 0
    ) {

        if (!fat_list_root(0))
            terminal_write(
                "Unable to read FAT16 root directory\n"
            );

    } else if (
        starts_with_local(cmd, "fatcat ")
    ) {

        const char *filename =
            cmd + 7;

        if (!fat_cat(0, filename))
            terminal_write(
                "Unable to read file\n"
            );

    } else if (
        strcmp_local(cmd, "reboot") == 0
    ) {

        terminal_write(
            "Rebooting system...\n"
        );

        outb(0x64, 0xFE);

    } else if (cmd[0] != '\0') {

        terminal_write(
            "Unknown command. Type 'help' for commands.\n"
        );
    }
}

void kernel_main(void) {
    char input_buf[128];

    terminal_write(
        "\n=== AngOS Command Shell ===\n"
    );

    terminal_write(
        "Type 'help' to get started.\n\n"
    );

    while (1) {

        terminal_write(
            "AngOS> "
        );

        keyboard_gets(
            input_buf,
            sizeof(input_buf)
        );

        execute_command(
            input_buf
        );
    }
}