#include <keyboard.h>
#include <terminal.h>
#include <cpu/IO.h>
#include <disk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <diagnostics.h>
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

static const char *strchr_local(const char *s, int c) {
    if (!s)
        return 0;

    for (;;) {
        if (*s == (char)c)
            return s;

        if (*s == '\0')
            return 0;

        ++s;
    }
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

static bool shell_make_fat83(
    const char *input,
    char output[11]
) {
    if (!input || !*input)
        return false;

    for (u32 i = 0; i < 11; ++i)
        output[i] = ' ';

    u32 base = 0;
    u32 ext = 0;
    bool dot = false;

    for (u32 i = 0; input[i]; ++i) {
        char c = input[i];

        if (c == '/')
            return false;

        if (c == '.') {
            if (dot)
                return false;

            dot = true;
            continue;
        }

        if (c < 32 || c == ' ')
            return false;

        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';

        if (!dot) {
            if (base >= 8)
                return false;

            output[base++] = c;
        } else {
            if (ext >= 3)
                return false;

            output[8 + ext++] = c;
        }
    }

    return base > 0;
}

static void shell_nw(
    const char *argument
) {
    if (!argument || !argument[0]) {
        terminal_write(
            "nw: missing filename\n"
        );
        return;
    }

    if (!disktable ||
        !volumes ||
        disktable[0].parts[0].fs != fat16) {

        terminal_write(
            "nw: FAT16 filesystem unavailable\n"
        );
        return;
    }

    char fat_name[11];

    if (!shell_make_fat83(
            argument,
            fat_name
        )) {

        terminal_write(
            "nw: invalid 8.3 filename\n"
        );
        return;
    }

    u8 mbr[512];
    u8 boot[512];
    u8 sector[512];

    if (!disk_read_sector(
            0,
            mbr,
            0
        )) {

        terminal_write(
            "nw: unable to read MBR\n"
        );
        return;
    }

    u32 partition_lba =
        (u32)mbr[446 + 8] |
        ((u32)mbr[446 + 9] << 8) |
        ((u32)mbr[446 + 10] << 16) |
        ((u32)mbr[446 + 11] << 24);

    if (!disk_read_sector(
            partition_lba,
            boot,
            0
        )) {

        terminal_write(
            "nw: unable to read FAT boot sector\n"
        );
        return;
    }

    u16 bytes_per_sector =
        (u16)boot[11] |
        ((u16)boot[12] << 8);

    u8 sectors_per_cluster =
        boot[13];

    u16 reserved_sectors =
        (u16)boot[14] |
        ((u16)boot[15] << 8);

    u8 fat_count =
        boot[16];

    u16 root_entries =
        (u16)boot[17] |
        ((u16)boot[18] << 8);

    u16 sectors_per_fat =
        (u16)boot[22] |
        ((u16)boot[23] << 8);

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0) {

        terminal_write(
            "nw: invalid FAT16 filesystem\n"
        );
        return;
    }

    u32 root_sectors =
        ((u32)root_entries * 32 + 511) / 512;

    u32 fat_lba =
        partition_lba +
        reserved_sectors;

    u32 root_lba =
        fat_lba +
        ((u32)fat_count * sectors_per_fat);

    for (u32 s = 0;
         s < root_sectors;
         ++s) {

        if (!disk_read_sector(
                root_lba + s,
                sector,
                0
            )) {

            terminal_write(
                "nw: unable to read root directory\n"
            );
            return;
        }

        for (u32 off = 0;
             off < 512;
             off += 32) {

            u8 first =
                sector[off];

            if (first == 0x00 ||
                first == 0xE5) {

                memset(
                    &sector[off],
                    0,
                    32
                );

                memcpy(
                    &sector[off],
                    fat_name,
                    11
                );

                sector[off + 11] =
                    ARCHIVE;
                if (!writesectorpio(root_lba + s, 1, sector, 0)) {
                    terminal_write("nw: disk write failed or timed out\n");
                    return;
                }

                u8 verify[512];

                if (!disk_read_sector(
                        root_lba + s,
                        verify,
                        0
                    )) {

                    terminal_write(
                        "nw: write verification failed\n"
                    );
                    return;
                }

                bool verified = true;

                for (u32 i = 0; i < 11; ++i) {
                    if (verify[off + i] !=
                        (u8)fat_name[i]) {

                        verified = false;
                        break;
                    }
                }

                if (!verified) {
                    terminal_write(
                        "nw: write verification failed\n"
                    );
                    return;
                }

                terminal_write(
                    "Created: "
                );

                terminal_write(
                    argument
                );

                terminal_write(
                    "\n"
                );

                return;
            }

            if (sector[off] == 0xE5)
                continue;

            bool same = true;

            for (u32 i = 0;
                 i < 11;
                 ++i) {

                if (sector[off + i] !=
                    (u8)fat_name[i]) {

                    same = false;
                    break;
                }
            }

            if (same) {
                terminal_write(
                    "nw: file already exists\n"
                );
                return;
            }
        }
    }

    terminal_write(
        "nw: root directory is full\n"
    );
}

static void shell_ec(const char *argument) {
    if (!argument || !argument[0]) {
        terminal_write("\n");
        return;
    }

    terminal_write(argument);
    terminal_write("\n");
}

static void shell_rm(const char *argument) {
    if (!argument) {
        terminal_write("Usage: rm <filename>\n");
        return;
    }

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    char filename[13];
    u32 length = 0;

    while (*argument &&
           *argument != ' ' &&
           *argument != '\t') {

        if (length >= sizeof(filename) - 1) {
            terminal_write("rm: filename is too long\n");
            return;
        }

        filename[length++] = *argument++;
    }

    if (length == 0) {
        terminal_write("Usage: rm <filename>\n");
        return;
    }

    filename[length] = '\0';

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    if (*argument) {
        terminal_write("Usage: rm <filename>\n");
        return;
    }

    char fat_name[11];

    if (!shell_make_fat83(filename, fat_name)) {
        terminal_write(
            "rm: invalid filename; use a FAT 8.3 filename\n"
        );
        return;
    }

    int result = fat16_delete_file(fat_name);

    if (result == FAT16_WRITE_OK) {
        terminal_write("rm: removed ");
        terminal_write(filename);
        terminal_write("\n");
    } else if (result == FAT16_WRITE_NOT_FOUND) {
        terminal_write("rm: file not found or entry changed\n");
    } else if (result == FAT16_WRITE_NOT_FILE) {
        terminal_write("rm: directories cannot be removed with this command\n");
    } else if (result == FAT16_WRITE_BAD_FS) {
        terminal_write("rm: a valid FAT16 filesystem is required\n");
    } else if (result == FAT16_WRITE_NO_MEMORY) {
        terminal_write("rm: not enough memory to validate the file\n");
    } else if (result == FAT16_WRITE_IO_ERROR) {
        terminal_write("rm: disk I/O or verification failed\n");
        terminal_write("rm: check the filesystem before continuing\n");
    } else if (result == FAT16_WRITE_DELETE_CLEANUP_ERROR) {
        terminal_write("rm: file entry removed, but cluster cleanup failed\n");
        terminal_write("rm: check the filesystem; some space may remain allocated\n");
    } else {
        terminal_write("rm: deletion failed\n");
    }
}

static void shell_cp(const char *argument) {
    char source_name[125];
    char destination_name[13];
    char source_path[128];
    char destination_path[16];
    char fat_name[11];

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    u32 source_length = 0;
    while (argument[source_length] &&
           argument[source_length] != ' ' &&
           argument[source_length] != '\t') {
        if (source_length >= sizeof(source_name) - 1) {
            terminal_write("cp: source path is too long\n");
            return;
        }
        source_name[source_length] = argument[source_length];
        ++source_length;
    }

    if (source_length == 0) {
        terminal_write("Usage: cp <source-file> <destination-name>\n");
        return;
    }
    source_name[source_length] = '\0';
    argument += source_length;

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    u32 destination_length = 0;
    while (argument[destination_length] &&
           argument[destination_length] != ' ' &&
           argument[destination_length] != '\t') {
        if (destination_length >= sizeof(destination_name) - 1) {
            terminal_write("cp: destination must be an 8.3 filename in the root directory\n");
            return;
        }
        destination_name[destination_length] = argument[destination_length];
        ++destination_length;
    }

    if (destination_length == 0) {
        terminal_write("Usage: cp <source-file> <destination-name>\n");
        return;
    }
    destination_name[destination_length] = '\0';
    argument += destination_length;

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    if (*argument) {
        terminal_write("Usage: cp <source-file> <destination-name>\n");
        return;
    }

    if (!shell_make_fat83(destination_name, fat_name) ||
        strchr_local(destination_name, '/') != 0) {
        terminal_write("cp: destination must be an 8.3 filename in the root directory\n");
        return;
    }

    source_path[0] = '|';
    source_path[1] = 'A';
    source_path[2] = '/';
    memcpy(source_path + 3, source_name, source_length + 1);

    destination_path[0] = '|';
    destination_path[1] = 'A';
    destination_path[2] = '/';
    memcpy(destination_path + 3, destination_name, destination_length + 1);

    DIR *source_dir = opendir(source_path);
    if (source_dir) {
        closedir(source_dir);
        terminal_write("cp: recursive directory copying is not supported yet\n");
        terminal_write("cp: directory creation and nested-file writing must be implemented first\n");
        return;
    }

    FILE *source_fp = fopen(source_path, 0);
    if (!source_fp) {
        terminal_write("cp: source file not found\n");
        return;
    }

    if (source_fp->size > 0xFFFFFFFFULL) {
        fclose(source_fp);
        terminal_write("cp: source file is too large\n");
        return;
    }

    u32 length = (u32)source_fp->size;
    u8 *buffer = malloc(length ? (size_t)length : 1);

    if (!buffer) {
        fclose(source_fp);
        terminal_write("cp: not enough memory\n");
        return;
    }

    u32 loaded = 0;
    while (loaded < length) {
        size_t count = fread(
            buffer + loaded,
            length - loaded,
            source_fp
        );

        if (count == 0)
            break;

        loaded += (u32)count;
    }

    fclose(source_fp);

    if (loaded != length) {
        free(buffer);
        terminal_write("cp: could not read the entire source file\n");
        return;
    }

    DIR *destination_dir = opendir(destination_path);
    if (destination_dir) {
        closedir(destination_dir);
        free(buffer);
        terminal_write("cp: destination is a directory, not a filename\n");
        return;
    }

    FILE *destination_fp = fopen(destination_path, 0);

    if (destination_fp) {
        fclose(destination_fp);
    } else {
        shell_nw(destination_name);
        destination_fp = fopen(destination_path, 0);

        if (!destination_fp) {
            free(buffer);
            terminal_write("cp: could not create destination file\n");
            return;
        }

        fclose(destination_fp);
    }

    int result = fat16_replace_file(fat_name, buffer, length);
    free(buffer);

    if (result == FAT16_WRITE_OK) {
        terminal_write("cp: copied ");
        terminal_write(source_name);
        terminal_write(" to ");
        terminal_write(destination_name);
        terminal_write("\n");
    } else if (result == FAT16_WRITE_NO_SPACE) {
        terminal_write("cp: not enough disk space\n");
    } else if (result == FAT16_WRITE_NO_MEMORY) {
        terminal_write("cp: not enough memory for disk operation\n");
    } else if (result == FAT16_WRITE_TOO_LARGE) {
        terminal_write("cp: file is too large for the destination operation\n");
    } else {
        terminal_write("cp: copy failed; the destination may be unchanged\n");
    }
}

#define ED_MAX_FILE_SIZE 16384U
#define ED_MAX_LINES 8192U

typedef struct {
    const char *text;
    u32 length;
} ed_line_t;

static ed_line_t ed_lines[ED_MAX_LINES + 1];

static void shell_ed(const char *argument) {
    if (!argument) {
        terminal_write("ed: missing arguments\n");
        return;
    }

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    char filename[13];
    u32 filename_length = 0;

    while (*argument &&
           *argument != ' ' &&
           *argument != '\t') {

        if (filename_length >= sizeof(filename) - 1) {
            terminal_write("ed: invalid filename\n");
            return;
        }

        filename[filename_length++] = *argument++;
    }

    if (filename_length == 0) {
        terminal_write(
            "Usage: ed <file> +N text | + text | +0 text | =N text | -N\n"
        );
        return;
    }

    filename[filename_length] = '\0';

    char fat_name[11];

    if (!shell_make_fat83(filename, fat_name)) {
        terminal_write("ed: invalid 8.3 filename\n");
        return;
    }

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    if (!*argument) {
        terminal_write("ed: missing action\n");
        return;
    }

    const char *token_start = argument;

    while (*argument &&
           *argument != ' ' &&
           *argument != '\t')
        ++argument;

    const char *token_end = argument;
    char action = token_start[0];

    if (action != '+' && action != '=' && action != '-') {
        terminal_write("ed: action must be +, = or -\n");
        return;
    }

    u32 line_number = 0;
    bool has_line_number = false;
    const char *number_start = token_start + 1;
    const char *number_end = token_end;

    if (number_start == token_end) {
        const char *next = token_end;
        while (*next == ' ' || *next == '\t')
            ++next;

        if (*next >= '0' && *next <= '9') {
            number_start = next;
            number_end = next;

            while (*number_end &&
                   *number_end != ' ' &&
                   *number_end != '\t')
                ++number_end;
        }
    }

    for (const char *p = number_start;
         p < number_end;
         ++p) {
        if (*p < '0' || *p > '9') {
            terminal_write("ed: invalid line number\n");
            return;
        }

        u32 digit = (u32)(*p - '0');

        if (line_number > (0xFFFFFFFFU - digit) / 10U) {
            terminal_write("ed: line number is too large\n");
            return;
        }

        line_number = line_number * 10U + digit;
        has_line_number = true;
    }

    if (action != '+' && !has_line_number) {
        terminal_write("ed: this action requires a line number\n");
        return;
    }

    argument = number_end;

    /* Skip spaces after the action/line number. */
    while (*argument == ' ' || *argument == '\t')
        ++argument;

    const char *payload = 0;
    u32 payload_length = 0;

    if (action == '-') {
        if (*argument) {
            terminal_write(
                "ed: delete takes no text; example: ed notes.txt -1\n"
            );
            return;
        }
    } else {
        if (*argument == '"') {
            /* Quoted text: preserve spaces inside the quotes. */
            payload = ++argument;

            while (*argument && *argument != '"')
                ++argument;

            if (*argument != '"') {
                terminal_write(
                    "ed: missing closing quote; use \"text\" or plain text\n"
                );
                return;
            }

            payload_length = (u32)(argument - payload);
            ++argument;

            while (*argument == ' ' || *argument == '\t')
                ++argument;

            if (*argument) {
                terminal_write(
                    "ed: unexpected text after closing quote\n"
                );
                return;
            }
        } else {
            /*
             * Plain text: quotes are optional.
             * Keep internal spaces and trim trailing spaces/tabs.
             */
            payload = argument;
            const char *payload_end = argument;

            while (*payload_end)
                ++payload_end;

            while (payload_end > payload &&
                   (payload_end[-1] == ' ' ||
                    payload_end[-1] == '\t'))
                --payload_end;

            payload_length = (u32)(payload_end - payload);

            if (payload_length == 0) {
                terminal_write(
                    "ed: missing text; use \"\" for an empty line\n"
                );
                return;
            }
        }
    }

    char path[16];

    path[0] = '|';
    path[1] = 'A';
    path[2] = '/';

    for (u32 i = 0; i < filename_length; ++i)
        path[i + 3] = filename[i];

    path[filename_length + 3] = '\0';

    FILE *fp = fopen(path, 0);

    if (!fp) {
        terminal_write(
            "ed: file not found; create it with nw first\n"
        );
        return;
    }

    if (fp->size > ED_MAX_FILE_SIZE) {
        fclose(fp);
        terminal_write("ed: file exceeds the 16 KiB editor limit\n");
        return;
    }

    u32 old_size = (u32)fp->size;
    u8 *old_data = malloc((size_t)old_size + 1);
    u8 *new_data = malloc(ED_MAX_FILE_SIZE + 1);

    const char *message = 0;
    bool saved = false;

    if (!old_data || !new_data) {
        message = "ed: not enough memory\n";
        goto cleanup;
    }

    u32 loaded = 0;

    while (loaded < old_size) {
        size_t count = fread(
            old_data + loaded,
            old_size - loaded,
            fp
        );

        if (count == 0)
            break;

        loaded += (u32)count;
    }

    fclose(fp);
    fp = 0;

    if (loaded != old_size) {
        message = "ed: unable to read the whole file\n";
        goto cleanup;
    }

    old_data[old_size] = '\0';

    bool had_final_newline =
        old_size > 0 && old_data[old_size - 1] == '\n';

    u32 line_count = 0;
    u32 start = 0;

    for (u32 i = 0; i < old_size; ++i) {
        if (old_data[i] != '\n')
            continue;

        if (line_count >= ED_MAX_LINES) {
            message = "ed: file contains too many lines\n";
            goto cleanup;
        }

        ed_lines[line_count].text =
            (const char *)old_data + start;

        ed_lines[line_count].length = i - start;

        ++line_count;
        start = i + 1;
    }

    if (start < old_size) {
        if (line_count >= ED_MAX_LINES) {
            message = "ed: file contains too many lines\n";
            goto cleanup;
        }

        ed_lines[line_count].text =
            (const char *)old_data + start;

        ed_lines[line_count].length = old_size - start;

        ++line_count;
    }

    if (action == '+') {
        if (line_count >= ED_MAX_LINES) {
            message = "ed: maximum line count reached\n";
            goto cleanup;
        }

        u32 index;

        if (!has_line_number) {
            index = line_count;
        } else if (line_number == 0) {
            index = 0;
        } else {
            index = line_number - 1;
        }

        if (index > line_count)
            index = line_count;

        for (u32 i = line_count; i > index; --i)
            ed_lines[i] = ed_lines[i - 1];

        ed_lines[index].text = payload;
        ed_lines[index].length = payload_length;

        ++line_count;
    }

    else if (action == '=') {
        if (line_count == 0 && line_number == 1) {
            ed_lines[0].text = payload;
            ed_lines[0].length = payload_length;
            line_count = 1;
            had_final_newline = false;
        } else {
            if (line_number == 0 || line_number > line_count) {
                message =
                    "ed: line does not exist; use + to append\n";
                goto cleanup;
            }

            ed_lines[line_number - 1].text = payload;
            ed_lines[line_number - 1].length = payload_length;
        }
    }

    else {
        if (line_number == 0 || line_number > line_count) {
            message =
                "ed: line does not exist; file unchanged\n";
            goto cleanup;
        }

        u32 index = line_number - 1;

        for (u32 i = index; i + 1 < line_count; ++i)
            ed_lines[i] = ed_lines[i + 1];

        --line_count;

        if (line_count == 0)
            had_final_newline = false;
    }

    bool output_final_newline =
        line_count > 0 &&
        (had_final_newline ||
         ed_lines[line_count - 1].length == 0);

    u32 new_size = 0;

    for (u32 i = 0; i < line_count; ++i) {
        if (i != 0) {
            if (new_size >= ED_MAX_FILE_SIZE) {
                message = "ed: edited file exceeds 16 KiB\n";
                goto cleanup;
            }

            ++new_size;
        }

        if (ed_lines[i].length >
            ED_MAX_FILE_SIZE - new_size) {

            message = "ed: edited file exceeds 16 KiB\n";
            goto cleanup;
        }

        new_size += ed_lines[i].length;
    }

    if (output_final_newline) {
        if (new_size >= ED_MAX_FILE_SIZE) {
            message = "ed: edited file exceeds 16 KiB\n";
            goto cleanup;
        }

        ++new_size;
    }

    u32 output_offset = 0;

    for (u32 i = 0; i < line_count; ++i) {
        if (i != 0)
            new_data[output_offset++] = '\n';

        if (ed_lines[i].length != 0) {
            memcpy(
                new_data + output_offset,
                ed_lines[i].text,
                ed_lines[i].length
            );

            output_offset += ed_lines[i].length;
        }
    }

    if (output_final_newline)
        new_data[output_offset++] = '\n';

    {
        int result = fat16_replace_file(
            fat_name,
            new_data,
            output_offset
        );

        if (result == FAT16_WRITE_OK) {
            saved = true;
        } else if (result == FAT16_WRITE_NOT_FOUND) {
            message = "ed: file not found\n";
        } else if (result == FAT16_WRITE_NO_SPACE) {
            message = "ed: not enough disk space\n";
        } else if (result == FAT16_WRITE_NO_MEMORY) {
            message = "ed: not enough memory for disk operation\n";
        } else if (result == FAT16_WRITE_BAD_FS) {
            message = "ed: FAT16 filesystem unavailable\n";
        } else if (result == FAT16_WRITE_NOT_FILE) {
            message = "ed: target is not a file\n";
        } else if (result == FAT16_WRITE_NOT_EMPTY) {
            message = "ed: file changed while editing; no changes committed\n";
        } else if (result == FAT16_WRITE_TOO_LARGE) {
            message = "ed: edited file is too large for this operation\n";
        } else if (result == FAT16_WRITE_IO_ERROR) {
            message = "ed: disk read, write, or verification failed\n";
        } else {
            message = "ed: saving the edited file failed\n";
        }
    }

cleanup:
    if (fp)
        fclose(fp);

    if (old_data)
        free(old_data);

    if (new_data)
        free(new_data);

    if (message)
        terminal_write(message);

    if (saved) {
        terminal_write("ed: saved ");
        terminal_write(filename);
        terminal_write("\n");
    }
}

static void shell_wt_status(void) {
    terminal_write("=== AngOS Watchtower Status ===\n");
    terminal_write("Diagnostics: ");
    terminal_write(diagnostics_is_initialized() ? "INITIALIZED\n" : "NOT INITIALIZED\n");
    terminal_write("Buffered events: ");
    print_u64_decimal(diagnostics_event_count());
    terminal_write(" / ");
    print_u64_decimal(diagnostics_event_capacity());
    terminal_write("\n");
}

static void shell_wt_log(u32 requested) {
    u32 count = diagnostics_event_count();
    u32 limit = requested;
    if (limit > diagnostics_event_capacity())
        limit = diagnostics_event_capacity();
    if (limit > count)
        limit = count;
    terminal_write("=== AngOS Watchtower Event History ===\n");
    if (count == 0) {
        terminal_write("No diagnostic events recorded.\n");
        return;
    }
    for (u32 i = count - limit; i < count; ++i) {
        char event[96];
        if (diagnostics_get_event(i, event, sizeof(event))) {
            terminal_write("[");
            print_u64_decimal((u64)i + 1);
            terminal_write("] ");
            terminal_write(event);
            terminal_write("\n");
        }
    }
}

static void shell_wt_release_blocks(void **blocks, u32 count) {
    while (count > 0) {
        --count;
        if (blocks[count]) {
            free(blocks[count]);
            blocks[count] = 0;
        }
    }
}

static bool shell_wt_memory_test(void) {
    const u32 sizes[] = {13, 507, 4096, 4103, 8192};
    const u32 block_count = sizeof(sizes) / sizeof(sizes[0]);
    u64 stable_free_pages = 0;

    for (u32 round = 0; round < 17; ++round) {
        void *blocks[5] = {0, 0, 0, 0, 0};
        bool passed = true;

        for (u32 i = 0; i < block_count; ++i) {
            blocks[i] = malloc(sizes[i]);
            if (!blocks[i]) {
                passed = false;
                break;
            }
        }

        if (passed) {
            for (u32 i = 0; i < block_count && passed; ++i) {
                for (u32 offset = 0; offset < sizes[i]; offset += PAGE_SIZE) {
                    if (!find_physaddr((u8 *)blocks[i] + offset)) {
                        passed = false;
                        break;
                    }
                }
                if (!find_physaddr((u8 *)blocks[i] + sizes[i] - 1))
                    passed = false;
            }
        }

        if (passed) {
            for (u32 i = 0; i < block_count; ++i) {
                u8 *bytes = (u8 *)blocks[i];
                for (u32 offset = 0; offset < sizes[i]; ++offset)
                    bytes[offset] = (u8)(0xA5u ^ round ^ i ^ (offset & 0xFFu));
            }

            for (u32 i = 0; i < block_count && passed; ++i) {
                u8 *bytes = (u8 *)blocks[i];
                for (u32 offset = 0; offset < sizes[i]; ++offset) {
                    u8 expected = (u8)(0xA5u ^ round ^ i ^ (offset & 0xFFu));
                    if (bytes[offset] != expected) {
                        passed = false;
                        break;
                    }
                }
            }
        }

        shell_wt_release_blocks(blocks, block_count);

        if (!passed) {
            diagnostics_log("Watchtower memory test: allocation, mapping or data verification failed.");
            terminal_write("Memory allocation and paging test: FAILED\n");
            return false;
        }

        u64 free_pages = phys_memory_free_pages();
        if (round == 0) {
            stable_free_pages = free_pages;
        } else if (free_pages != stable_free_pages) {
            diagnostics_log("Watchtower memory test: free-page count changed after a repeated allocation cycle.");
            terminal_write("Memory allocation and paging test: FAILED\n");
            terminal_write("Free-page count was not stable across repeated cycles.\n");
            return false;
        }
    }

    for (u32 round = 0; round < 32; ++round) {
        u64 before = phys_memory_free_pages();
        void *page = phys_page_alloc();
        if (!page || (((u64)page & (PAGE_SIZE - 1)) != 0)) {
            if (page)
                phys_page_free(page);
            diagnostics_log("Watchtower memory test: physical page allocation failed or returned an unaligned page.");
            terminal_write("Physical page allocation test: FAILED\n");
            return false;
        }
        if (phys_memory_free_pages() + 1 != before) {
            phys_page_free(page);
            diagnostics_log("Watchtower memory test: free-page counter did not decrease after allocation.");
            terminal_write("Physical page accounting test: FAILED\n");
            return false;
        }
        phys_page_free(page);
        if (phys_memory_free_pages() != before) {
            diagnostics_log("Watchtower memory test: physical page was not returned to the allocator.");
            terminal_write("Physical page release test: FAILED\n");
            return false;
        }
    }

    diagnostics_log("Watchtower memory test: allocation, mapping, data verification and physical page reuse passed.");
    terminal_write("Memory allocation test: PASS\n");
    terminal_write("Virtual mapping test: PASS\n");
    terminal_write("Physical page reuse test: PASS\n");
    terminal_write("Repeated cycles: 17 heap cycles, 32 physical page cycles\n");
    return true;
}

static bool shell_wt_filesystem_test(void) {
    u64 blocks = 0;
    u8 sector[512];

    if (!disk_probe(&blocks)) {
        diagnostics_log("Watchtower filesystem test: primary disk probe failed.");
        terminal_write("Disk probe: FAILED\n");
        return false;
    }

    if (!disk_read_sector(0, sector, 0)) {
        diagnostics_log("Watchtower filesystem test: sector-zero read failed.");
        terminal_write("Sector read: FAILED\n");
        return false;
    }

    int type = fat_detect(0);
    if (type != 12 && type != 16 && type != 32) {
        diagnostics_log("Watchtower filesystem test: no supported FAT filesystem was detected.");
        terminal_write("Filesystem detection: FAILED\n");
        return false;
    }
    if (type != 16) {
        diagnostics_log("Watchtower filesystem test: root-directory traversal currently requires FAT16 support.");
        terminal_write("Filesystem detected, but root-directory testing currently requires FAT16.\n");
        return false;
    }

    DIR *dir = opendir("|A/");
    if (!dir) {
        diagnostics_log("Watchtower filesystem test: root directory could not be opened.");
        terminal_write("Root directory open: FAILED\n");
        return false;
    }

    u32 entries = 0;
    dirent *entry;
    while ((entry = readdir(dir)) != 0) {
        ++entries;
        if (entries >= 4096) {
            closedir(dir);
            diagnostics_log("Watchtower filesystem test: root directory scan exceeded the safety limit.");
            terminal_write("Root directory scan: FAILED\n");
            return false;
        }
    }
    closedir(dir);

    terminal_write("Disk probe: PASS\n");
    terminal_write("Sector-zero read: PASS\n");
    terminal_write("Filesystem: FAT");
    if (type == 12)
        terminal_write("12\n");
    else if (type == 16)
        terminal_write("16\n");
    else
        terminal_write("32\n");
    terminal_write("Root directory read: PASS\n");
    terminal_write("Root entries read: ");
    print_u64_decimal(entries);
    terminal_write("\nDisk sectors reported: ");
    print_u64_decimal(blocks);
    terminal_write("\n");
    diagnostics_log("Watchtower filesystem test: disk probe, sector read, FAT detection and root-directory traversal passed.");
    return true;
}
static void shell_wt(const char *argument) {
    char command[64];
    u32 length = 0;
    if (!argument)
        argument = "";
    while (argument[length] && length < sizeof(command) - 1) {
        command[length] = argument[length];
        ++length;
    }
    if (argument[length]) {
        terminal_write("wt: command is too long. Type 'h wt' for usage.\n");
        return;
    }
    command[length] = '\0';
    u32 start = 0;
    while (command[start] == ' ' || command[start] == '\t')
        ++start;
    while (length > start && (command[length - 1] == ' ' || command[length - 1] == '\t'))
        command[--length] = '\0';
    const char *sub = command + start;
    if (!*sub || strcmp_local(sub, "status") == 0) {
        shell_wt_status();
        return;
    }
    if (strcmp_local(sub, "log") == 0) {
        shell_wt_log(10);
        return;
    }
    if (starts_with_local(sub, "log ")) {
        const char *p = sub + 4;
        while (*p == ' ' || *p == '\t')
            ++p;
        u32 limit = 0;
        bool valid = *p >= '0' && *p <= '9';
        while (*p >= '0' && *p <= '9') {
            u32 digit = (u32)(*p - '0');
            if (limit > 100000) {
                valid = false;
                break;
            }
            limit = limit * 10 + digit;
            ++p;
        }
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!valid || *p || limit == 0) {
            terminal_write("Usage: wt log [count]\n");
            return;
        }
        shell_wt_log(limit);
        return;
    }
    if (strcmp_local(sub, "clear") == 0) {
        diagnostics_clear_events();
        terminal_write("Watchtower in-memory event history cleared.\n");
        return;
    }
    if (strcmp_local(sub, "selftest") == 0) {
        terminal_write("Running Watchtower event-history self-test...\n");
        if (diagnostics_selftest())
            terminal_write("Watchtower self-test: PASS\n");
        else
            terminal_write("Watchtower self-test: FAILED\n");
        return;
    }
    if (strcmp_local(sub, "memtest") == 0) {
        terminal_write("Running repeated memory allocation, mapping and release tests...\n");
        shell_wt_memory_test();
        return;
    }
    if (strcmp_local(sub, "fscheck") == 0) {
        terminal_write("Running read-only disk and filesystem checks...\n");
        if (!shell_wt_filesystem_test())
            terminal_write("Filesystem check: FAILED\n");
        else
            terminal_write("Filesystem check: PASS\n");
        return;
    }
    if (strcmp_local(sub, "report") == 0) {
        terminal_write("=== AngOS Watchtower Report ===\n");
        shell_wt_status();
        shell_wt_log(10);
        terminal_write("Fatal panic details are handled by the kernel panic reporter.\n");
        return;
    }
    terminal_write("wt: unknown subcommand. Type 'h wt' for usage.\n");
}

static void shell_help(const char *topic) {
    if (!topic)
        topic = "";

    while (*topic == ' ' || *topic == '\t')
        ++topic;

    if (!*topic) {
        terminal_write("ANGOS COMMAND REFERENCE\n");
        terminal_write("Use: h [command] to list commands or display detailed help.\n");
        terminal_write("Notation: <required> must be supplied; [optional] may be omitted.\n");
        terminal_write("          Do not type the angle brackets or square brackets.\n\n");
        terminal_write("Command   Usage                  Description\n");
        terminal_write("--------  ---------------------  ------------------------------\n");
        terminal_write("h         h [command]            Show general or command-specific help\n");
        terminal_write("cls       cls                    Clear the terminal\n");
        terminal_write("ver       ver                    Display the OS version\n");
        terminal_write("disk      disk                   Detect the primary disk\n");
        terminal_write("diskread  diskread               Read disk sector zero\n");
        terminal_write("diskinit  diskinit               Initialize disk and filesystem tables\n");
        terminal_write("fstest    fstest                 Test file reading\n");
        terminal_write("ls        ls [directory]         List a directory\n");
        terminal_write("ct        ct <file>              Display a file\n");
        terminal_write("stat      stat <path>            Show file or directory details\n");
        terminal_write("nw        nw <filename>          Create an empty file\n");
        terminal_write("ec        ec [text]              Print text or a blank line\n");
        terminal_write("ed        ed <file> <action> [text] Edit or remove one line\n");
        terminal_write("cp        cp <source-file> <destination-name> Copy a file\n");
        terminal_write("rm        rm <filename>          Delete a file\n");
        terminal_write("wt        wt [subcommand]        Inspect diagnostics and run checks\n");
        terminal_write("fat       fat                    Detect the filesystem type\n");
        terminal_write("fatls     fatls                  Legacy FAT directory listing\n");
        terminal_write("fatcat    fatcat <file>          Legacy FAT file reader\n");
        terminal_write("reboot    reboot                 Restart the machine\n");
        return;
    }

    if (strcmp_local(topic, "h") == 0 ||
        strcmp_local(topic, "help") == 0) {
        terminal_write("Command : h (short for help)\n");
        terminal_write("Purpose : Display the command reference or detailed help for one command.\n");
        terminal_write("Usage   : h [command]\n");
        terminal_write("Details : With no argument, h displays the command reference.\n");
        terminal_write("          Supply a command name to view its purpose, usage, options and\n");
        terminal_write("          examples. The reference explains required and optional arguments.\n");
        terminal_write("Example : h ed\n");

    } else if (strcmp_local(topic, "cls") == 0) {
        terminal_write("Command : cls (clear screen)\n");
        terminal_write("Purpose : Remove the current terminal output from view.\n");
        terminal_write("Usage   : cls\n");
        terminal_write("Details : This clears the display; it does not delete files or reset AngOS.\n");
        terminal_write("Example : cls\n");

    } else if (strcmp_local(topic, "ver") == 0) {
        terminal_write("Command : ver (short for version)\n");
        terminal_write("Purpose : Display the AngOS kernel version string.\n");
        terminal_write("Usage   : ver\n");
        terminal_write("Details : Useful for checking which kernel build is running.\n");
        terminal_write("Example : ver\n");

    } else if (strcmp_local(topic, "disk") == 0) {
        terminal_write("Command : disk\n");
        terminal_write("Purpose : Probe the primary ATA disk.\n");
        terminal_write("Usage   : disk\n");
        terminal_write("Details : Reports whether a disk is detected and, when available,\n");
        terminal_write("          its reported sector count. This is a diagnostic command.\n");
        terminal_write("Example : disk\n");

    } else if (strcmp_local(topic, "diskread") == 0) {
        terminal_write("Command : diskread (disk read)\n");
        terminal_write("Purpose : Read and inspect disk sector zero.\n");
        terminal_write("Usage   : diskread\n");
        terminal_write("Details : Reports whether the read succeeded and displays the final\n");
        terminal_write("          two bytes, which are commonly the boot-sector signature.\n");
        terminal_write("Example : diskread\n");

    } else if (strcmp_local(topic, "diskinit") == 0) {
        terminal_write("Command : diskinit (disk initialize)\n");
        terminal_write("Purpose : Initialize AngOS disk, partition and filesystem tables.\n");
        terminal_write("Usage   : diskinit\n");
        terminal_write("Details : Reports the detected filesystem type where available.\n");
        terminal_write("          Run this when disk or filesystem information is uninitialized.\n");
        terminal_write("Example : diskinit\n");

    } else if (strcmp_local(topic, "fstest") == 0) {
        terminal_write("Command : fstest (filesystem test)\n");
        terminal_write("Purpose : Test opening and reading a known bootloader file.\n");
        terminal_write("Usage   : fstest\n");
        terminal_write("Details : Prints file-open and read results for diagnosing filesystem\n");
        terminal_write("          API problems. It is a diagnostic test, not a repair tool.\n");
        terminal_write("Example : fstest\n");

    } else if (strcmp_local(topic, "ls") == 0) {
        terminal_write("Command : ls (short for list)\n");
        terminal_write("Purpose : List directory entries.\n");
        terminal_write("Usage   : ls [directory]\n");
        terminal_write("Details : With no argument, lists the root directory. A path can be\n");
        terminal_write("          supplied to list a directory relative to volume A.\n");
        terminal_write("Example : ls EFI/BOOT\n");

    } else if (strcmp_local(topic, "ct") == 0) {
        terminal_write("Command : ct (compact file-display command)\n");
        terminal_write("Purpose : Display readable text from a file without editing it.\n");
        terminal_write("Usage   : ct <file>\n");
        terminal_write("Details : This compact command is inspired by cat, short for\n");
        terminal_write("          concatenate. AngOS currently displays one file per command.\n");
        terminal_write("Example : ct notes.txt\n");

    } else if (strcmp_local(topic, "stat") == 0) {
        terminal_write("Command : stat (short for status)\n");
        terminal_write("Purpose : Report whether a path is a file or directory.\n");
        terminal_write("Usage   : stat <path>\n");
        terminal_write("Details : For files, displays the file size in bytes. It does not\n");
        terminal_write("          modify the file or its contents.\n");
        terminal_write("Example : stat notes.txt\n");

    } else if (strcmp_local(topic, "nw") == 0) {
        terminal_write("Command : nw (short for new)\n");
        terminal_write("Purpose : Create a new, empty file.\n");
        terminal_write("Usage   : nw <filename>\n");
        terminal_write("Details : Creates a file in the FAT16 root directory. The current\n");
        terminal_write("          implementation uses FAT 8.3 filenames. It will not overwrite\n");
        terminal_write("          an existing entry with the same filename.\n");
        terminal_write("Example : nw notes.txt\n");

    } else if (strcmp_local(topic, "ec") == 0) {
        terminal_write("Command : ec (short for echo)\n");
        terminal_write("Purpose : Print the provided text to the terminal.\n");
        terminal_write("Usage   : ec [text]\n");
        terminal_write("Details : With text, ec prints it to the screen. With no text,\n");
        terminal_write("          it prints a blank line. It never creates or saves a file.\n");
        terminal_write("Example : ec Hello from AngOS\n");

    } else if (strcmp_local(topic, "ed") == 0) {
        terminal_write("Command : ed (short for edit)\n");
        terminal_write("Purpose : Insert, replace or delete one line in an existing file.\n");
        terminal_write("Usage   : ed <file> <action> [text]\n");
        terminal_write("Details : The file and action are required. Text is needed for + and =\n");
        terminal_write("          actions, but must be omitted for - actions. Line numbers start\n");
        terminal_write("          at 1. Use ordinary text or put it in double quotes.\n\n");
        terminal_write("  ed <file> +<line> <text> Insert text before line <line>\n");
        terminal_write("  ed <file> + text     Append a line at the end\n");
        terminal_write("  ed <file> +0 text    Insert at the beginning\n");
        terminal_write("  ed <file> =<line> <text> Replace line <line>\n");
        terminal_write("  ed <file> -<line>       Delete line <line>\n\n");
        terminal_write("Details : +<line> inserts before that line. If the line is beyond the\n");
        terminal_write("          end of the file, text is appended. +0 inserts at the start.\n");
        terminal_write("          =<line> replaces an existing line; -<line> deletes that line.\n");
        terminal_write("          Type digits for the line number, not the word line or brackets.\n");
        terminal_write("          Deletion takes no text. Ranges and multi-line editing are not\n");
        terminal_write("          supported.\n\n");
        terminal_write("Examples: ed notes.txt + First line\n");
        terminal_write("          ed notes.txt +2 A new second line\n");
        terminal_write("          ed notes.txt =1 Updated first line\n");
        terminal_write("          ed notes.txt -1\n");
        terminal_write("          ed notes.txt +1 \"Text with spaces\"\n");
        terminal_write("          ed notes.txt +1 \"\"\n");
        terminal_write("Details : Create the file with nw first. The editor has a 16 KiB\n");
        terminal_write("          file-size limit in the current implementation.\n");

    } else if (strcmp_local(topic, "cp") == 0) {
        terminal_write("Command : cp (short for copy)\n");
        terminal_write("Purpose : Copy a file to a different root-directory filename.\n");
        terminal_write("Usage   : cp <source-file> <destination-name>\n");
        terminal_write("Details : The source may be in a subdirectory. The destination must\n");
        terminal_write("          be a FAT 8.3 filename in the root directory. Recursive\n");
        terminal_write("          directory copying is not supported.\n");
        terminal_write("Example : cp notes.txt backup.txt\n");

    } else if (strcmp_local(topic, "rm") == 0) {
        terminal_write("Command : rm (short for remove)\n");
        terminal_write("Purpose : Delete a file entry and release its FAT16 cluster chain.\n");
        terminal_write("Usage   : rm <filename>\n");
        terminal_write("Details : Currently supports FAT16 root-directory files with FAT 8.3\n");
        terminal_write("          filenames. Directories and paths are not supported.\n");
        terminal_write("          Deletion is permanent; there is no recycle bin or undo.\n");
        terminal_write("          If disk I/O fails, inspect the filesystem before continuing.\n");
        terminal_write("Example : rm notes.txt\n");
    } else if (strcmp_local(topic, "wt") == 0 || strcmp_local(topic, "watchtower") == 0) {
        terminal_write("Command : wt (AngOS Watchtower)\n");
        terminal_write("Alias   : watchtower\n");
        terminal_write("Purpose : Inspect system diagnostics, recent events, memory handling\n");
        terminal_write("          and read-only disk and filesystem checks.\n");
        terminal_write("Usage   : wt [subcommand]\n");
        terminal_write("Details : With no subcommand, wt displays status. The in-memory event\n");
        terminal_write("          history stores up to 32 events and is cleared on reboot.\n");
        terminal_write("          The history is for events recorded while the kernel is running.\n\n");
        terminal_write("Argument notation: <required> must be supplied; [optional] may be\n");
        terminal_write("          omitted. Do not type the brackets themselves.\n\n");
        terminal_write("Subcommands:\n");
        terminal_write("  wt status\n");
        terminal_write("      Show whether diagnostics initialized and how many events are\n");
        terminal_write("      buffered. This command does not change system state.\n\n");
        terminal_write("  wt log [count]\n");
        terminal_write("      Display recent events from oldest to newest. Count is optional;\n");
        terminal_write("      it defaults to 10 and is capped at the 32 events held in memory.\n");
        terminal_write("      Count must be a positive decimal number.\n\n");
        terminal_write("  wt clear\n");
        terminal_write("      Clear only the in-memory event history. This does not change\n");
        terminal_write("      disk contents or alter any separately captured diagnostic output.\n\n");
        terminal_write("  wt selftest\n");
        terminal_write("      Record a test event and verify that Watchtower can retrieve it.\n");
        terminal_write("      The test event remains in the history until it is overwritten or cleared.\n\n");
        terminal_write("  wt memtest\n");
        terminal_write("      Repeat heap allocations, memory writes, data verification, virtual\n");
        terminal_write("      mapping checks, and physical page allocation and release. This test\n");
        terminal_write("      changes temporary memory contents but does not intentionally write files.\n\n");
        terminal_write("  wt fscheck\n");
        terminal_write("      Run read-only disk probing, sector-zero reading, FAT detection and\n");
        terminal_write("      root-directory traversal. It does not create, edit or delete files.\n\n");
        terminal_write("  wt report\n");
        terminal_write("      Display the current status, up to 10 recent events and a note about\n");
        terminal_write("      how fatal panic details are handled. It does not save a report to disk.\n\n");
        terminal_write("Examples:\n");
        terminal_write("  wt\n");
        terminal_write("  wt status\n");
        terminal_write("  wt log 20\n");
        terminal_write("  wt selftest\n");
        terminal_write("  wt memtest\n");
        terminal_write("  wt fscheck\n");
        terminal_write("  wt report\n");
        terminal_write("  wt clear\n");

    } else if (strcmp_local(topic, "fat") == 0) {
        terminal_write("Command : fat\n");
        terminal_write("Purpose : Identify the supported FAT filesystem type.\n");
        terminal_write("Usage   : fat\n");
        terminal_write("Details : Reports the filesystem type detected by AngOS. This is a\n");
        terminal_write("          detection command and does not format the volume.\n");
        terminal_write("Example : fat\n");

    } else if (strcmp_local(topic, "fatls") == 0) {
        terminal_write("Command : fatls (legacy FAT listing)\n");
        terminal_write("Purpose : Run the older FAT16 root-directory listing routine.\n");
        terminal_write("Usage   : fatls\n");
        terminal_write("Details : Kept for compatibility and filesystem diagnostics. Use ls\n");
        terminal_write("          for normal directory listings.\n");
        terminal_write("Example : fatls\n");

    } else if (strcmp_local(topic, "fatcat") == 0) {
        terminal_write("Command : fatcat (legacy FAT file reader)\n");
        terminal_write("Purpose : Read a file using the older FAT16 reader.\n");
        terminal_write("Usage   : fatcat <filename>\n");
        terminal_write("Details : Retained for diagnostics and compatibility. Use ct for\n");
        terminal_write("          normal file display.\n");
        terminal_write("Example : fatcat notes.txt\n");

    } else if (strcmp_local(topic, "reboot") == 0) {
        terminal_write("Command : reboot\n");
        terminal_write("Purpose : Request a machine restart.\n");
        terminal_write("Usage   : reboot\n");
        terminal_write("Details : This sends a reset request through the keyboard controller.\n");
        terminal_write("          Save important work first; unsaved volatile state is lost.\n");
        terminal_write("Example : reboot\n");

    } else {
        terminal_write("ERROR   : No detailed help is available for that command.\n");
        terminal_write("Usage   : h <command>\n");
    }
}

void execute_command(const char *cmd) {
    if (strcmp_local(cmd, "h") == 0 ||
        strcmp_local(cmd, "help") == 0) {
        shell_help("");
    } else if (starts_with_local(cmd, "h ")) {
        shell_help(cmd + 2);
    } else if (starts_with_local(cmd, "help ")) {
        shell_help(cmd + 5);
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

    } else if (strcmp_local(cmd, "ct") == 0) {

        terminal_write("Usage: ct <file>\\n");

    } else if (
        starts_with_local(cmd, "ct ")
    ) {

        const char *name =
            cmd + 3;

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
                "ct: file not found\n"
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
        starts_with_local(cmd, "stat ")
    ) {

        shell_stat(
            cmd + 5
        );

    } else if (
        starts_with_local(cmd, "nw ")
    ) {

        shell_nw(
            cmd + 3
        );

    } else if (
        strcmp_local(cmd, "ec") == 0
    ) {

        shell_ec("");

    } else if (
        starts_with_local(cmd, "ec ")
    ) {

        shell_ec(cmd + 3);

    } else if (
        strcmp_local(cmd, "ed") == 0
    ) {

        shell_ed("");

    } else if (
        starts_with_local(cmd, "ed ")
    ) {

        shell_ed(cmd + 3);

    } else if (
        strcmp_local(cmd, "cp") == 0
    ) {

        shell_cp("");

    } else if (
        starts_with_local(cmd, "cp ")
    ) {

        shell_cp(cmd + 3);

    } else if (strcmp_local(cmd, "rm") == 0) {
        shell_rm("");
    } else if (starts_with_local(cmd, "rm ")) {
        shell_rm(cmd + 3);
    } else if (strcmp_local(cmd, "wt") == 0 || strcmp_local(cmd, "watchtower") == 0) {
        shell_wt("");
    } else if (starts_with_local(cmd, "wt ")) {
        shell_wt(cmd + 3);
    } else if (starts_with_local(cmd, "watchtower ")) {
        shell_wt(cmd + 11);

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
            "Unknown command. Type 'h' for commands.\n"
        );
    }
}

void kernel_main(void) {
    char input_buf[128];

    terminal_write(
        "\n=== AngOS Command Shell ===\n"
    );

    terminal_write(
        "Type 'h' to get started.\n\n"
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