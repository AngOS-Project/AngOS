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

FILE *fopen(char *path, u8 flags) {
    void *oldpath = path;
    unsigned int i, j = 0;
    FILE *fp = files;

    (void)flags;

    unsigned int dirs;

    for (dirs = 0; *path; ++path) {
        if (*path == '/')
            ++dirs;
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
            fp->size =
                fat_filesize(fp);
            break;

        default:
            goto open_error;
    }

    return fp;

open_error:
    fp->used = 0;

error:
    for (i = 0; i < dirs; ++i) {
        if (fpath[i])
            free(fpath[i]);
    }

    free(fpath);

    return 0;
}

int aligncheck(FILE *fp) {
    if (fp < files ||
        fp >= files + MAX_OPEN_FILES)
        return 1;

    if ((unsigned long long)fp %
            sizeof(FILE) !=
        (unsigned long long)files %
            sizeof(FILE))
        return 2;

    return 0;
}

int fclose(FILE *fp) {
    if (aligncheck(fp))
        return 1;

    fp->used = 0;

    char **ptr = fp->path;

    while (*ptr != NULL)
        free(*ptr++);

    free(fp->path);

    fp->path = NULL;
    fp->pointer = 0;
    fp->size = 0;

    return 0;
}

size_t fread(
    void *ptr,
    size_t bytes,
    FILE *fp
) {
    if (!ptr || !fp)
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
    if (!fp)
        return -1;

    switch (position) {

        case SEEK_SET:
            if (offset < 0 ||
                (unsigned long long)offset >
                fp->size)
                return -2;

            fp->pointer = offset;
            break;

        case SEEK_CUR: {
            long long next =
                (long long)fp->pointer +
                offset;

            if (next < 0 ||
                (unsigned long long)next >
                fp->size)
                return -2;

            fp->pointer = next;
            break;
        }

        case SEEK_END:
            if (offset > 0)
                return -2;

            if ((unsigned long long)(-offset) >
                fp->size)
                return -2;

            fp->pointer =
                fp->size + offset;
            break;

        default:
            return -1;
    }

    return 0;
}

void rewind(FILE *fp) {
    if (aligncheck(fp))
        return;

    fp->pointer = 0;
}

unsigned long long fsize(FILE *fp) {
    if (aligncheck(fp))
        return 0;

    return fp->size;
}

DIR *opendir(char *path) {
    if (!path)
        return 0;

    if (path[0] != '|' ||
        path[1] < 'A' ||
        path[1] > 'Z' ||
        path[2] != '/')
        return 0;

    u8 volume =
        (u8)(path[1] - 'A');

    for (u32 i = 0;
         i < MAX_OPEN_DIRS;
         ++i) {

        if (dir_table[i].used)
            continue;

        DIR *dir =
            &dir_table[i];

        memset(dir, 0, sizeof(DIR));

        dir->used = 1;
        dir->volume = volume;

        unsigned int disk =
            volumes[volume].disk;

        unsigned int part =
            volumes[volume].partition;

        dir->fs =
            (u8)disktable[disk]
                .parts[part]
                .fs;

        if (dir->fs == fat16) {
            if (!fat16_dir_open(
                    dir,
                    path)) {

                dir->used = 0;
                return 0;
            }

            return dir;
        }

        dir->used = 0;
        return 0;
    }

    return 0;
}

dirent *readdir(DIR *dir) {
    if (!dir ||
        dir < dir_table ||
        dir >= dir_table + MAX_OPEN_DIRS ||
        !dir->used)
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
    if (!dir ||
        dir < dir_table ||
        dir >= dir_table + MAX_OPEN_DIRS ||
        !dir->used)
        return 1;

    dir->used = 0;

    return 0;
}
