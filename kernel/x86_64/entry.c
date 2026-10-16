static const char *shell_normalize_command(const char *command);

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
            "list: unable to open directory\n"
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

    if (dir->reserved) {
        terminal_write("list: directory scan failed because disk reading failed.\n");
        closedir(dir);
        return;
    }
    closedir(dir);
}

static void shell_stat(
    const char *argument
) {
    if (!argument || argument[0] == '\0') {
        terminal_write(
            "status: missing path\n"
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
        "status: path not found\n"
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
            "new: missing filename\n"
        );
        return;
    }

    if (!disktable ||
        !volumes ||
        disktable[0].parts[0].fs != fat16) {

        terminal_write(
            "new: FAT16 filesystem unavailable\n"
        );
        return;
    }

    char fat_name[11];

    if (!shell_make_fat83(
            argument,
            fat_name
        )) {

        terminal_write(
            "new: invalid 8.3 filename\n"
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
            "new: unable to read MBR\n"
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
            "new: unable to read FAT boot sector\n"
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
            "new: invalid FAT16 filesystem\n"
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
                "new: unable to read root directory\n"
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
                    terminal_write("new: disk write failed or timed out\n");
                    return;
                }

                u8 verify[512];

                if (!disk_read_sector(
                        root_lba + s,
                        verify,
                        0
                    )) {

                    terminal_write(
                        "new: write verification failed\n"
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
                        "new: write verification failed\n"
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
                    "new: file already exists\n"
                );
                return;
            }
        }
    }

    terminal_write(
        "new: root directory is full\n"
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
        terminal_write("Usage: remove <filename>\n");
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
            terminal_write("remove: filename is too long\n");
            return;
        }

        filename[length++] = *argument++;
    }

    if (length == 0) {
        terminal_write("Usage: remove <filename>\n");
        return;
    }

    filename[length] = '\0';

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    if (*argument) {
        terminal_write("Usage: remove <filename>\n");
        return;
    }

    char fat_name[11];

    if (!shell_make_fat83(filename, fat_name)) {
        terminal_write(
            "remove: invalid filename; use a FAT 8.3 filename\n"
        );
        return;
    }

    int result = fat16_delete_file(fat_name);

    if (result == FAT16_WRITE_OK) {
        terminal_write("remove: removed ");
        terminal_write(filename);
        terminal_write("\n");
    } else if (result == FAT16_WRITE_NOT_FOUND) {
        terminal_write("remove: file not found or entry changed\n");
    } else if (result == FAT16_WRITE_NOT_FILE) {
        terminal_write("remove: directories cannot be removed with this command\n");
    } else if (result == FAT16_WRITE_BAD_FS) {
        terminal_write("remove: a valid FAT16 filesystem is required\n");
    } else if (result == FAT16_WRITE_NO_MEMORY) {
        terminal_write("remove: not enough memory to validate the file\n");
    } else if (result == FAT16_WRITE_IO_ERROR) {
        terminal_write("remove: disk I/O or verification failed\n");
        terminal_write("remove: check the filesystem before continuing\n");
    } else if (result == FAT16_WRITE_DELETE_CLEANUP_ERROR) {
        terminal_write("remove: file entry removed, but cluster cleanup failed\n");
        terminal_write("remove: check the filesystem; some space may remain allocated\n");
    } else {
        terminal_write("remove: deletion failed\n");
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
            terminal_write("copy: source path is too long\n");
            return;
        }
        source_name[source_length] = argument[source_length];
        ++source_length;
    }

    if (source_length == 0) {
        terminal_write("Usage: copy <source-file> <destination-name>\n");
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
            terminal_write("copy: destination must be an 8.3 filename in the root directory\n");
            return;
        }
        destination_name[destination_length] = argument[destination_length];
        ++destination_length;
    }

    if (destination_length == 0) {
        terminal_write("Usage: copy <source-file> <destination-name>\n");
        return;
    }
    destination_name[destination_length] = '\0';
    argument += destination_length;

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    if (*argument) {
        terminal_write("Usage: copy <source-file> <destination-name>\n");
        return;
    }

    if (!shell_make_fat83(destination_name, fat_name) ||
        strchr_local(destination_name, '/') != 0) {
        terminal_write("copy: destination must be an 8.3 filename in the root directory\n");
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
        terminal_write("copy: recursive directory copying is not supported yet\n");
        terminal_write("copy: directory creation and nested-file writing must be implemented first\n");
        return;
    }

    FILE *source_fp = fopen(source_path, 0);
    if (!source_fp) {
        terminal_write("copy: source file not found\n");
        return;
    }

    if (source_fp->size > 0xFFFFFFFFULL) {
        fclose(source_fp);
        terminal_write("copy: source file is too large\n");
        return;
    }

    u32 length = (u32)source_fp->size;
    u8 *buffer = malloc(length ? (size_t)length : 1);

    if (!buffer) {
        fclose(source_fp);
        terminal_write("copy: not enough memory\n");
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
        terminal_write("copy: could not read the entire source file\n");
        return;
    }

    DIR *destination_dir = opendir(destination_path);
    if (destination_dir) {
        closedir(destination_dir);
        free(buffer);
        terminal_write("copy: destination is a directory, not a filename\n");
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
            terminal_write("copy: could not create destination file\n");
            return;
        }

        fclose(destination_fp);
    }

    int result = fat16_replace_file(fat_name, buffer, length);
    free(buffer);

    if (result == FAT16_WRITE_OK) {
        terminal_write("copy: copied ");
        terminal_write(source_name);
        terminal_write(" to ");
        terminal_write(destination_name);
        terminal_write("\n");
    } else if (result == FAT16_WRITE_NO_SPACE) {
        terminal_write("copy: not enough disk space\n");
    } else if (result == FAT16_WRITE_NO_MEMORY) {
        terminal_write("copy: not enough memory for disk operation\n");
    } else if (result == FAT16_WRITE_TOO_LARGE) {
        terminal_write("copy: file is too large for the destination operation\n");
    } else {
        terminal_write("copy: copy failed; the destination may be unchanged\n");
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
        terminal_write("edit: missing arguments\n");
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
            terminal_write("edit: invalid filename\n");
            return;
        }

        filename[filename_length++] = *argument++;
    }

    if (filename_length == 0) {
        terminal_write(
            "Usage: edit <file> +N text | + text | +0 text | =N text | -N\n"
        );
        return;
    }

    filename[filename_length] = '\0';

    char fat_name[11];

    if (!shell_make_fat83(filename, fat_name)) {
        terminal_write("edit: invalid 8.3 filename\n");
        return;
    }

    while (*argument == ' ' || *argument == '\t')
        ++argument;

    if (!*argument) {
        terminal_write("edit: missing action\n");
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
        terminal_write("edit: action must be +, = or -\n");
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
            terminal_write("edit: invalid line number\n");
            return;
        }

        u32 digit = (u32)(*p - '0');

        if (line_number > (0xFFFFFFFFU - digit) / 10U) {
            terminal_write("edit: line number is too large\n");
            return;
        }

        line_number = line_number * 10U + digit;
        has_line_number = true;
    }

    if (action != '+' && !has_line_number) {
        terminal_write("edit: this action requires a line number\n");
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
                "edit: delete takes no text; example: ed notes.txt -1\n"
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
                    "edit: missing closing quote; use \"text\" or plain text\n"
                );
                return;
            }

            payload_length = (u32)(argument - payload);
            ++argument;

            while (*argument == ' ' || *argument == '\t')
                ++argument;

            if (*argument) {
                terminal_write(
                    "edit: unexpected text after closing quote\n"
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
                    "edit: missing text; use \"\" for an empty line\n"
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
            "edit: file not found; create it with nw first\n"
        );
        return;
    }

    if (fp->size > ED_MAX_FILE_SIZE) {
        fclose(fp);
        terminal_write("edit: file exceeds the 16 KiB editor limit\n");
        return;
    }

    u32 old_size = (u32)fp->size;
    u8 *old_data = malloc((size_t)old_size + 1);
    u8 *new_data = malloc(ED_MAX_FILE_SIZE + 1);

    const char *message = 0;
    bool saved = false;

    if (!old_data || !new_data) {
        message = "edit: not enough memory\n";
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
        message = "edit: unable to read the whole file\n";
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
            message = "edit: file contains too many lines\n";
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
            message = "edit: file contains too many lines\n";
            goto cleanup;
        }

        ed_lines[line_count].text =
            (const char *)old_data + start;

        ed_lines[line_count].length = old_size - start;

        ++line_count;
    }

    if (action == '+') {
        if (line_count >= ED_MAX_LINES) {
            message = "edit: maximum line count reached\n";
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
                    "edit: line does not exist; use + to append\n";
                goto cleanup;
            }

            ed_lines[line_number - 1].text = payload;
            ed_lines[line_number - 1].length = payload_length;
        }
    }

    else {
        if (line_number == 0 || line_number > line_count) {
            message =
                "edit: line does not exist; file unchanged\n";
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
                message = "edit: edited file exceeds 16 KiB\n";
                goto cleanup;
            }

            ++new_size;
        }

        if (ed_lines[i].length >
            ED_MAX_FILE_SIZE - new_size) {

            message = "edit: edited file exceeds 16 KiB\n";
            goto cleanup;
        }

        new_size += ed_lines[i].length;
    }

    if (output_final_newline) {
        if (new_size >= ED_MAX_FILE_SIZE) {
            message = "edit: edited file exceeds 16 KiB\n";
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
            message = "edit: file not found\n";
        } else if (result == FAT16_WRITE_NO_SPACE) {
            message = "edit: not enough disk space\n";
        } else if (result == FAT16_WRITE_NO_MEMORY) {
            message = "edit: not enough memory for disk operation\n";
        } else if (result == FAT16_WRITE_BAD_FS) {
            message = "edit: FAT16 filesystem unavailable\n";
        } else if (result == FAT16_WRITE_NOT_FILE) {
            message = "edit: target is not a file\n";
        } else if (result == FAT16_WRITE_NOT_EMPTY) {
            message = "edit: file changed while editing; no changes committed\n";
        } else if (result == FAT16_WRITE_TOO_LARGE) {
            message = "edit: edited file is too large for this operation\n";
        } else if (result == FAT16_WRITE_IO_ERROR) {
            message = "edit: disk read, write, or verification failed\n";
        } else {
            message = "edit: saving the edited file failed\n";
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
        terminal_write("edit: saved ");
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
    if (dir->reserved) {
        closedir(dir);
        diagnostics_log("Watchtower filesystem test: directory traversal stopped after a disk read failure.");
        terminal_write("Root directory read: FAILED\n");
        return false;
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
typedef struct {
    const char *full;
    const char *alias;
} shell_alias_pair_t;

static const shell_alias_pair_t shell_alias_pairs[] = {
    {"help", "h"},
    {"clear", "cls"},
    {"version", "ver"},
    {"list", "ls"},
    {"cat", "ct"},
    {"status", "stat"},
    {"new", "nw"},
    {"echo", "ec"},
    {"edit", "ed"},
    {"copy", "cp"},
    {"remove", "rm"},
    {"watchtower", "wt"}
};

static const char *shell_expand_first_word(const char *input, char *output, size_t capacity, bool watchtower_subcommand) {
    if (!input)
        input = "";
    while (*input == ' ' || *input == '\t')
        ++input;
    const char *end = input;
    while (*end && *end != ' ' && *end != '\t')
        ++end;
    size_t word_length = (size_t)(end - input);
    if (word_length == 0 || word_length >= 32 || capacity == 0)
        return input;
    char word[32];
    memcpy(word, input, word_length);
    word[word_length] = '\0';
    const char *replacement = 0;
    if (watchtower_subcommand) {
        if (strcmp_local(word, "lg") == 0)
            replacement = "log";
        else if (strcmp_local(word, "cls") == 0)
            replacement = "clear";
        else if (strcmp_local(word, "rp") == 0)
            replacement = "report";
    } else {
        for (u32 i = 0; i < sizeof(shell_alias_pairs) / sizeof(shell_alias_pairs[0]); ++i) {
            if (strcmp_local(word, shell_alias_pairs[i].full) == 0) {
                replacement = shell_alias_pairs[i].alias;
                break;
            }
        }
    }
    if (!replacement)
        return input;
    size_t replacement_length = 0;
    while (replacement[replacement_length])
        ++replacement_length;
    size_t suffix_length = 0;
    while (end[suffix_length])
        ++suffix_length;
    if (replacement_length + suffix_length >= capacity)
        return input;
    memcpy(output, replacement, replacement_length);
    memcpy(output + replacement_length, end, suffix_length + 1);
    return output;
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
        terminal_write("watchtower: subcommand is too long. Type 'h wt' for usage.\n");
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
    if (strcmp_local(sub, "log") == 0 || strcmp_local(sub, "lg") == 0) {
        shell_wt_log(10);
        return;
    }
    if (starts_with_local(sub, "log ") || starts_with_local(sub, "lg ")) {
        const char *p = starts_with_local(sub, "lg ") ? sub + 3 : sub + 4;
        while (*p == ' ' || *p == '\t')
            ++p;
        u32 limit = 0;
        bool valid = *p >= '0' && *p <= '9';
        while (*p >= '0' && *p <= '9') {
            u32 digit = (u32)(*p - '0');
            if (limit > (0xFFFFFFFFu - digit) / 10) {
                valid = false;
                break;
            }
            limit = limit * 10 + digit;
            ++p;
        }
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!valid || *p || limit == 0) {
            terminal_write("Usage: watchtower log [count] | wt lg [count]\n");
            return;
        }
        shell_wt_log(limit);
        return;
    }
    if (strcmp_local(sub, "clear") == 0 || strcmp_local(sub, "cls") == 0) {
        diagnostics_clear_events();
        terminal_write("watchtower: in-memory event history cleared.\n");
        return;
    }
    if (strcmp_local(sub, "selftest") == 0) {
        terminal_write("watchtower: running the event-history self-test.\n");
        terminal_write(diagnostics_selftest() ? "watchtower self-test: PASS\n" : "watchtower self-test: FAILED\n");
        return;
    }
    if (strcmp_local(sub, "memtest") == 0) {
        terminal_write("watchtower: running repeated memory and paging tests.\n");
        shell_wt_memory_test();
        return;
    }
    if (strcmp_local(sub, "fscheck") == 0) {
        terminal_write("watchtower: running read-only disk and filesystem checks.\n");
        if (!shell_wt_filesystem_test())
            terminal_write("watchtower filesystem check: FAILED\n");
        else
            terminal_write("watchtower filesystem check: PASS\n");
        return;
    }
    if (strcmp_local(sub, "report") == 0 || strcmp_local(sub, "rp") == 0) {
        terminal_write("=== AngOS Watchtower Report ===\n");
        shell_wt_status();
        shell_wt_log(10);
        terminal_write("Fatal panic details are handled by the kernel panic reporter.\n");
        return;
    }
    terminal_write("watchtower: unknown subcommand. Type 'h wt' for usage.\n");
}

static const char *shell_normalize_command(const char *command) {
    static char normalized[256];
    char input[256];
    if (!command)
        command = "";
    u32 n = 0;
    while (command[n] && n < sizeof(input) - 1) {
        input[n] = command[n];
        ++n;
    }
    if (command[n])
        return command;
    input[n] = '\0';
    u32 end = 0;
    while (input[end] && input[end] != ' ' && input[end] != '\t')
        ++end;
    static const char *full_names[] = {"help", "clear", "version", "list", "cat", "status", "new", "echo", "edit", "copy", "remove", "watchtower"};
    static const char *aliases[] = {"h", "cls", "ver", "ls", "ct", "stat", "nw", "ec", "ed", "cp", "rm", "wt"};
    for (u32 k = 0; k < sizeof(full_names) / sizeof(full_names[0]); ++k) {
        u32 length = 0;
        while (full_names[k][length])
            ++length;
        if (length != end)
            continue;
        u32 j = 0;
        while (j < length && input[j] == full_names[k][j])
            ++j;
        if (j != length)
            continue;
        u32 out = 0;
        while (aliases[k][out]) {
            normalized[out] = aliases[k][out];
            ++out;
        }
        for (u32 i = end; input[i] && out < sizeof(normalized) - 1; ++i)
            normalized[out++] = input[i];
        normalized[out] = '\0';
        return normalized;
    }
    for (u32 i = 0; i <= n; ++i)
        normalized[i] = input[i];
    return normalized;
}

static void shell_help(const char *topic) {
    char expanded_topic[64];
    topic = shell_expand_first_word(topic, expanded_topic, sizeof(expanded_topic), false);
    if (!topic)
        topic = "";
    while (*topic == ' ' || *topic == '\t')
        ++topic;

    topic = shell_normalize_command(topic);
    if (!*topic) {
        terminal_write("ANGOS COMMAND REFERENCE\n");
        terminal_write("Use: help [command] | h [command] for detailed help.\n");
        terminal_write("Notation: <required> must be supplied; [optional] may be omitted.\n");
        terminal_write("          Do not type the angle brackets or square brackets.\n\n");
        terminal_write("COMMAND    ALIAS    USAGE                          DESCRIPTION\n");
        terminal_write("---------- -------- ------------------------------ ------------------------\n");
        terminal_write("help       h        help [command]                 Show detailed help\n");
        terminal_write("clear      cls      clear                          Clear terminal output\n");
        terminal_write("version    ver      version                        Show kernel version\n");
        terminal_write("disk                disk                           Probe primary disk\n");
        terminal_write("diskread            diskread                       Read disk sector zero\n");
        terminal_write("diskinit            diskinit                       Initialize disk tables\n");
        terminal_write("fstest              fstest                         Test file reading\n");
        terminal_write("list       ls       list [directory]               List directory entries\n");
        terminal_write("cat        ct       cat <file>                     Display file contents\n");
        terminal_write("status     stat     status <path>                  Show file or directory\n");
        terminal_write("new        nw       new <filename>                 Create an empty file\n");
        terminal_write("echo       ec       echo [text]                    Print text or a blank line\n");
        terminal_write("edit       ed       edit <file> <action> [text]    Edit one file line\n");
        terminal_write("copy       cp       copy <source> <destination>   Copy a file\n");
        terminal_write("remove     rm       remove <filename>              Delete a file\n");
        terminal_write("watchtower wt       watchtower [subcommand]         Inspect diagnostics\n");
        terminal_write("fat                 fat                            Detect supported FAT type\n");
        terminal_write("fatls               fatls                          Legacy FAT listing\n");
        terminal_write("fatcat              fatcat <filename>              Legacy FAT reader\n");
        terminal_write("reboot              reboot                         Restart the machine\n");
        return;
    }
    if (strcmp_local(topic, "h") == 0) {
        terminal_write("Command : help | h\n");
        terminal_write("Purpose : Show the complete command reference or help for one command.\n");
        terminal_write("Usage: help [command] | h [command]\n");
        terminal_write("Details : Omit command to display the full command list. Supply either\n");
        terminal_write("          the full command name or its listed alias for detailed help.\n");
        terminal_write("Example : help edit\n");
        terminal_write("          h ed\n");
    } else if (strcmp_local(topic, "cls") == 0) {
        terminal_write("Command : clear | cls\n");
        terminal_write("Purpose : Clear the current terminal display without restarting AngOS.\n");
        terminal_write("Usage: clear | cls\n");
        terminal_write("Example : clear\n");
        terminal_write("          cls\n");
    } else if (strcmp_local(topic, "ver") == 0) {
        terminal_write("Command : version | ver\n");
        terminal_write("Purpose : Display the AngOS version and architecture string.\n");
        terminal_write("Usage: version | ver\n");
        terminal_write("Example : version\n");
    } else if (strcmp_local(topic, "disk") == 0) {
        terminal_write("Command : disk\n");
        terminal_write("Purpose : Probe the primary ATA disk and display its reported capacity.\n");
        terminal_write("Usage: disk\n");
        terminal_write("Details : This reports whether the primary disk responds to identification.\n");
        terminal_write("Example : disk\n");
    } else if (strcmp_local(topic, "diskread") == 0) {
        terminal_write("Command : diskread\n");
        terminal_write("Purpose : Read disk sector zero and display its signature information.\n");
        terminal_write("Usage: diskread\n");
        terminal_write("Details : This is a read-only diagnostic command. It does not change the disk.\n");
        terminal_write("Example : diskread\n");
    } else if (strcmp_local(topic, "diskinit") == 0) {
        terminal_write("Command : diskinit\n");
        terminal_write("Purpose : Initialize the disk, partition and filesystem tables.\n");
        terminal_write("Usage: diskinit\n");
        terminal_write("Details : Reports the filesystem type detected for the primary partition.\n");
        terminal_write("Example : diskinit\n");
    } else if (strcmp_local(topic, "fstest") == 0) {
        terminal_write("Command : fstest\n");
        terminal_write("Purpose : Test opening and reading a known bootloader file.\n");
        terminal_write("Usage: fstest\n");
        terminal_write("Details : Use this to investigate basic file-open and file-read behaviour.\n");
        terminal_write("          It is a diagnostic test and does not repair the filesystem.\n");
        terminal_write("Example : fstest\n");
    } else if (strcmp_local(topic, "ls") == 0) {
        terminal_write("Command : list | ls\n");
        terminal_write("Purpose : Display the entries in a directory.\n");
        terminal_write("Usage: list [directory] | ls [directory]\n");
        terminal_write("Details : If directory is omitted, the root directory on volume A is listed.\n");
        terminal_write("Example : list\n");
        terminal_write("          ls EFI/BOOT\n");
    } else if (strcmp_local(topic, "ct") == 0) {
        terminal_write("Command : cat | ct\n");
        terminal_write("Purpose : Display readable text from one file without editing it.\n");
        terminal_write("Usage: cat <file> | ct <file>\n");
        terminal_write("Details : The file argument is required. Output is sent to the terminal.\n");
        terminal_write("Example : cat notes.txt\n");
        terminal_write("          ct notes.txt\n");
    } else if (strcmp_local(topic, "stat") == 0) {
        terminal_write("Command : status | stat\n");
        terminal_write("Purpose : Report whether a path is a file or directory, and show file size.\n");
        terminal_write("Usage: status <path> | stat <path>\n");
        terminal_write("Details : The path is required. This command does not modify the target.\n");
        terminal_write("Example : status notes.txt\n");
        terminal_write("          stat notes.txt\n");
    } else if (strcmp_local(topic, "nw") == 0) {
        terminal_write("Command : new | nw\n");
        terminal_write("Purpose : Create a new empty file.\n");
        terminal_write("Usage: new <filename> | nw <filename>\n");
        terminal_write("Details : The filename is required. The current implementation supports\n");
        terminal_write("          FAT 8.3 filenames in the FAT16 root directory and does not overwrite\n");
        terminal_write("          an existing file with the same name.\n");
        terminal_write("Example : new notes.txt\n");
        terminal_write("          nw notes.txt\n");
    } else if (strcmp_local(topic, "ec") == 0) {
        terminal_write("Command : echo | ec\n");
        terminal_write("Purpose : Print text to the terminal.\n");
        terminal_write("Usage: echo [text] | ec [text]\n");
        terminal_write("Details : Text is optional. With no text, the command prints a blank line.\n");
        terminal_write("          The text is displayed only; it is not saved to a file.\n");
        terminal_write("Example : echo Hello from AngOS\n");
        terminal_write("          ec Hello from AngOS\n");
    } else if (strcmp_local(topic, "ed") == 0) {
        terminal_write("Command : edit | ed\n");
        terminal_write("Purpose : Insert, replace or delete one line in an existing text file.\n");
        terminal_write("Usage: edit <file> <action> [text] | ed <file> <action> [text]\n");
        terminal_write("Details : The file and action are required. Text is required for insert\n");
        terminal_write("          and replace actions, and must be omitted for delete actions.\n");
        terminal_write("          Line numbers start at 1. Do not type the placeholder brackets.\n\n");
        terminal_write("Actions:\n");
        terminal_write("  edit <file> +<line> <text>  Insert before the specified line\n");
        terminal_write("  edit <file> + <text>        Append a line at the end\n");
        terminal_write("  edit <file> +0 <text>       Insert at the beginning\n");
        terminal_write("  edit <file> =<line> <text>  Replace an existing line\n");
        terminal_write("  edit <file> -<line>         Delete an existing line\n\n");
        terminal_write("Details : The line placeholder is replaced with digits, such as 1 or 2.\n");
        terminal_write("          Plain text can contain spaces. Double quotes are optional unless\n");
        terminal_write("          needed to represent an empty line. The file must exist first.\n");
        terminal_write("          The editor has a 16 KiB file-size limit and does not support ranges.\n\n");
        terminal_write("Examples:\n");
        terminal_write("  edit notes.txt + First line\n");
        terminal_write("  ed notes.txt +2 A new second line\n");
        terminal_write("  edit notes.txt =1 Updated first line\n");
        terminal_write("  edit notes.txt -1\n");
        terminal_write("  ed notes.txt +1 \"Text with spaces\"\n");
    } else if (strcmp_local(topic, "cp") == 0) {
        terminal_write("Command : copy | cp\n");
        terminal_write("Purpose : Copy the contents of one file to a new root-directory file.\n");
        terminal_write("Usage: copy <source-file> <destination-name> | cp <source-file> <destination-name>\n");
        terminal_write("Details : Both arguments are required. The source can be in a subdirectory;\n");
        terminal_write("          the destination is a FAT 8.3 filename in the root directory.\n");
        terminal_write("          Directory copying is not supported.\n");
        terminal_write("Example : copy notes.txt backup.txt\n");
        terminal_write("          cp notes.txt backup.txt\n");
    } else if (strcmp_local(topic, "rm") == 0) {
        terminal_write("Command : remove | rm\n");
        terminal_write("Purpose : Delete a file entry and release its FAT16 cluster chain.\n");
        terminal_write("Usage: remove <filename> | rm <filename>\n");
        terminal_write("Details : The filename is required. Current support is limited to FAT16\n");
        terminal_write("          root-directory files with FAT 8.3 names. Directories are not supported.\n");
        terminal_write("          Deletion is permanent. If disk I/O fails, inspect the filesystem.\n");
        terminal_write("Example : remove notes.txt\n");
        terminal_write("          rm notes.txt\n");
        } else if (strcmp_local(topic, "wt") == 0) {
        terminal_write("Command : watchtower | wt\n");
        terminal_write("Purpose : Diagnose AngOS, inspect recent events and run health checks.\n");
        terminal_write("Usage: watchtower [subcommand] | wt [subcommand]\n");
        terminal_write("Details : Watchtower records up to 32 diagnostic events in memory.\n");
        terminal_write("          The history is cleared on reboot and does not replace a saved report.\n\n");
        terminal_write("Notation: <required> must be supplied; [optional] may be omitted.\n");
        terminal_write("          Do not type the brackets themselves.\n\n");
        terminal_write("Subcommands:\n");
        terminal_write("  watchtower status | wt status\n");
        terminal_write("      Show diagnostic initialization and the number of buffered events.\n\n");
        terminal_write("  watchtower log [count] | wt lg [count]\n");
        terminal_write("      Display recent events from oldest to newest. Count defaults to 10\n");
        terminal_write("      and is capped at the 32 events held in memory. Count must be positive.\n\n");
        terminal_write("  watchtower clear | wt cls\n");
        terminal_write("      Clear only the in-memory event history.\n\n");
        terminal_write("  watchtower selftest | wt selftest\n");
        terminal_write("      Record and verify a test event in the event history.\n\n");
        terminal_write("  watchtower memtest | wt memtest\n");
        terminal_write("      Exercise repeated heap allocations, data integrity, virtual mappings\n");
        terminal_write("      and physical-page allocation and release.\n\n");
        terminal_write("  watchtower fscheck | wt fscheck\n");
        terminal_write("      Run read-only disk, filesystem and root-directory checks.\n\n");
        terminal_write("  watchtower report | wt rp\n");
        terminal_write("      Show current diagnostic status and the 10 most recent events.\n\n");
        terminal_write("Examples:\n");
        terminal_write("  watchtower log 20\n");
        terminal_write("  wt lg 10\n");
        terminal_write("  watchtower cls\n");
        terminal_write("  wt rp\n");

    } else if (strcmp_local(topic, "fat") == 0) {
        terminal_write("Command : fat\n");
        terminal_write("Purpose : Detect the FAT filesystem type on the primary disk.\n");
        terminal_write("Usage: fat\n");
        terminal_write("Details : This reports the detected type; it does not format the volume.\n");
        terminal_write("Example : fat\n");
    } else if (strcmp_local(topic, "fatls") == 0) {
        terminal_write("Command : fatls\n");
        terminal_write("Purpose : Run the legacy FAT16 root-directory listing routine.\n");
        terminal_write("Usage: fatls\n");
        terminal_write("Details : Retained for compatibility and low-level filesystem diagnostics.\n");
        terminal_write("          Use list or ls for ordinary directory listings.\n");
        terminal_write("Example : fatls\n");
    } else if (strcmp_local(topic, "fatcat") == 0) {
        terminal_write("Command : fatcat\n");
        terminal_write("Purpose : Read a file using the legacy FAT16 reader.\n");
        terminal_write("Usage: fatcat <filename>\n");
        terminal_write("Details : Retained for compatibility and diagnostics. Use cat or ct for\n");
        terminal_write("          ordinary file display.\n");
        terminal_write("Example : fatcat notes.txt\n");
    } else if (strcmp_local(topic, "reboot") == 0) {
        terminal_write("Command : reboot\n");
        terminal_write("Purpose : Request a machine restart.\n");
        terminal_write("Usage: reboot\n");
        terminal_write("Details : Save important work first; unsaved volatile state is lost.\n");
        terminal_write("Example : reboot\n");
    } else {
        terminal_write("No detailed help is available for that command.\n");
        terminal_write("Usage: help [command] or h [command]\n");
    }
}
void execute_command(const char *cmd) {
    cmd = shell_normalize_command(cmd);
    char expanded_command[128];
    cmd = shell_expand_first_word(cmd, expanded_command, sizeof(expanded_command), false);

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

        terminal_write("Usage: cat <file>\\n");

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