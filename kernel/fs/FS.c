#include <types.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <FS.h>
#include "FAT.h"

#define MAX_OPEN_DIRS 32

static FILE file_table[MAX_OPEN_FILES];
FILE *files = file_table;

static DIR dir_table[MAX_OPEN_DIRS];

static int fs_dir_slot_status(const DIR *dir) {
    if (!dir)
        return 1;
    u64 address = (u64)dir;
    u64 base = (u64)dir_table;
    u64 table_size = (u64)sizeof(DIR) * MAX_OPEN_DIRS;
    if (address < base)
        return 1;
    u64 offset = address - base;
    if (offset >= table_size)
        return 1;
    if (offset % sizeof(DIR) != 0)
        return 2;
    return 0;
}

static bool fs_dir_is_open(const DIR *dir) {
    return fs_dir_slot_status(dir) == 0 && dir->used != 0 && dir->volume < LETTERS;
}

static int fs_file_slot_status(const FILE *fp) {
    if (!fp || !files)
        return 1;
    u64 address = (u64)fp;
    u64 base = (u64)files;
    u64 offset;
    u64 table_size = (u64)sizeof(FILE) * MAX_OPEN_FILES;
    if (address < base)
        return 1;
    offset = address - base;
    if (offset >= table_size)
        return 1;
    if (offset % sizeof(FILE) != 0)
        return 2;
    return 0;
}

static bool fs_file_is_open(const FILE *fp) {
    if (fs_file_slot_status(fp) != 0)
        return false;
    return fp->used != 0 && fp->path != 0 && fp->path_count != 0;
}

FILE *fopen(char *path, u8 flags) {
    void *oldpath = path;
    unsigned int i, j = 0;
    FILE *fp = files;

    (void)flags;

    if (!path)
        return 0;

    unsigned int dirs = 0;
    u32 path_length = 0;

    while (path[path_length]) {
        if (path_length >= FILENAME_LENGTH - 1)
            return 0;
        if (path[path_length] == '/') {
            ++dirs;
            if (dirs >= 16)
                return 0;
        }
        ++path_length;
    }

    if (dirs == 0)
        return 0;

    path = oldpath;

    char **fpath =
        malloc((dirs + 1) * sizeof(char *));

    if (!fpath)
        return 0;

    for (i = 0; i < dirs; ++i) {
        fpath[i] = malloc(FILENAME_LENGTH);

        if (!fpath[i]) {
            while (i > 0)
                free(fpath[--i]);

            free(fpath);
            return 0;
        }
    }

    fpath[dirs] = NULL;

    if (*path++ != '|')
        goto error;

    u8 volume =
        (u8)(*path++ - 'A');

    if (volume >= LETTERS)
        goto error;

    if (*path++ != '/')
        goto error;

    while (1) {
        i = 0;

        while (*path != '/') {
            if (*path == '\0') {
                fpath[j][i] = '\0';
                goto main_1;
            }

            if (i >= FILENAME_LENGTH - 1)
                goto error;

            fpath[j][i++] = *path++;
        }

        fpath[j][i] = '\0';

        ++j;
        ++path;

        if (j >= dirs)
            goto error;
    }

main_1:

    for (i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!files[i].used)
            break;
    }

    if (i >= MAX_OPEN_FILES)
        goto error;

    fp = &files[i];

    fp->used = 1;
    fp->path = fpath;
    fp->path_count = dirs;
    fp->pointer = 0;
    fp->volume = volume;
    fp->size = 0;

    unsigned int disk =
        volumes[volume].disk;

    unsigned int part =
        volumes[volume].partition;

    switch (
        disktable[disk]
        .parts[part]
        .fs
    ) {
        case fat16: {
            FAT_entry entry;

            if (!fat16_find_file_entry(
                    fp,
                    &entry))
                goto open_error;

            fp->size = entry.size;
            break;
        }

        case fat32:
            if (!fat_filesize(fp, &fp->size))
                goto open_error;
            break;

        default:
            goto open_error;
    }

    return fp;

open_error:
    fp->used = 0;
    fp->path = 0;
    fp->path_count = 0;
    fp->pointer = 0;
    fp->size = 0;

error:
    for (i = 0; i < dirs; ++i) {
        if (fpath[i])
            free(fpath[i]);
    }

    free(fpath);

    return 0;
}

int aligncheck(FILE *fp) {
    if (!fp || !files)
        return 1;
    u64 address = (u64)fp;
    u64 base = (u64)files;
    u64 table_size = (u64)sizeof(FILE) * MAX_OPEN_FILES;
    if (address < base)
        return 1;
    u64 offset = address - base;
    if (offset >= table_size)
        return 1;
    if (offset % sizeof(FILE) != 0)
        return 2;
    return 0;
}

int fclose(FILE *fp) {
    if (!fs_file_is_open(fp))
        return 1;

    fp->used = 0;
    for (u32 i = 0; i < fp->path_count; ++i) {
        if (fp->path[i])
            free(fp->path[i]);
    }
    free(fp->path);
    fp->path = 0;
    fp->path_count = 0;
    fp->pointer = 0;
    fp->volume = 0;
    fp->size = 0;
    return 0;
}

size_t fread(
    void *ptr,
    size_t bytes,
    FILE *fp
) {
    if (!ptr || !fs_file_is_open(fp) || !disktable || !volumes || fp->volume >= LETTERS)
        return 0;

    if (bytes == 0)
        return 0;

    if (fp->pointer >= fp->size)
        return 0;

    if (bytes >
        fp->size - fp->pointer)
        bytes =
            fp->size - fp->pointer;

    unsigned int disk =
        volumes[fp->volume].disk;

    unsigned int part =
        volumes[fp->volume].partition;

    switch (
        disktable[disk]
        .parts[part]
        .fs
    ) {
        case fat16:
            return fat16_read(
                ptr,
                bytes,
                fp
            );

        case fat32:
            return fat_read(
                ptr,
                bytes,
                fp
            );

        default:
            return 0;
    }
}

int fseek(
    FILE *fp,
    long long offset,
    fpos_t position
) {
    if (!fs_file_is_open(fp))
        return -1;
    if (fp->pointer > fp->size)
        return -2;

    switch (position) {

        case SEEK_SET:
            if (offset < 0 ||
                (unsigned long long)offset >
                fp->size)
                return -2;

            fp->pointer = offset;
            break;

        case SEEK_CUR:
            if (offset < 0) {
                u64 distance = (u64)(-(offset + 1)) + 1;
                if (distance > fp->pointer)
                    return -2;
                fp->pointer -= distance;
            } else {
                u64 distance = (u64)offset;
                if (distance > fp->size - fp->pointer)
                    return -2;
                fp->pointer += distance;
            }
            break;

        case SEEK_END:
            if (offset > 0)
                return -2;
            {
                u64 distance = (u64)(-(offset + 1)) + 1;
                if (distance > fp->size)
                    return -2;
                fp->pointer = fp->size - distance;
            }
            break;

        default:
            return -1;
    }

    return 0;
}

void rewind(FILE *fp) {
    if (!fs_file_is_open(fp))
        return;

    fp->pointer = 0;
}

unsigned long long fsize(FILE *fp) {
    if (!fs_file_is_open(fp))
        return 0;

    return fp->size;
}

DIR *opendir(char *path) {
    if (!path || !disktable || !volumes)
        return 0;

    if (path[0] != '|' ||
        path[1] < 'A' ||
        path[1] > 'Z' ||
        path[2] != '/')
        return 0;

    u8 volume = (u8)(path[1] - 'A');
    if (volume >= LETTERS)
        return 0;

    unsigned int disk = volumes[volume].disk;
    unsigned int part = volumes[volume].partition;
    if (disk >= 4 || part >= 4)
        return 0;

    for (u32 i = 0; i < MAX_OPEN_DIRS; ++i) {
        if (dir_table[i].used)
            continue;

        DIR *dir = &dir_table[i];
        memset(dir, 0, sizeof(DIR));
        dir->used = 1;
        dir->volume = volume;
        dir->fs = (u8)disktable[disk].parts[part].fs;

        if (dir->fs == fat16) {
            if (!fat16_dir_open(dir, path)) {
                memset(dir, 0, sizeof(DIR));
                return 0;
            }
            return dir;
        }

        memset(dir, 0, sizeof(DIR));
        return 0;
    }

    return 0;
}

dirent *readdir(DIR *dir) {
    if (!fs_dir_is_open(dir))
        return 0;

    switch (dir->fs) {
        case fat16:
            if (fat16_dir_read(dir))
                return &dir->entry;
            return 0;
        default:
            return 0;
    }
}

int closedir(DIR *dir) {
    if (!fs_dir_is_open(dir))
        return 1;

    memset(dir, 0, sizeof(DIR));
    return 0;
}
