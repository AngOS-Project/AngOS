#include <types.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <FS.h>
#include "FAT.h"

static FILE file_table[MAX_OPEN_FILES];
FILE *files = file_table;

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

    u8 volume = (u8)(*path++ - 'A');

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

    unsigned int disk = volumes[volume].disk;
    unsigned int part = volumes[volume].partition;

    switch (disktable[disk].parts[part].fs) {
        case fat16: {
            FAT_entry entry;

            if (!fat16_find_file_entry(fp, &entry))
                goto open_error;

            fp->size = entry.size;
            break;
        }

        case fat32:
            fp->size = fat_filesize(fp);
            break;

        default:
            goto open_error;
    }

    return fp;

open_error:
    fp->used = 0;

error:
    if (fpath) {
        for (i = 0; i < dirs; ++i) {
            if (fpath[i])
                free(fpath[i]);
        }

        free(fpath);
    }

    return 0;
}

int aligncheck(FILE *fp) {
	// Boundary check
	if (fp < files || fp >= files + MAX_OPEN_FILES) {
		return 1;
	}
	// Lined-up check
	if ((unsigned long long) fp % sizeof(FILE) != (unsigned long long) files % sizeof(FILE)) {
		return 2;
	}

	return 0;
}

int fclose(FILE *fp) {
	if (aligncheck(fp)) return 1;
	fp->used = 0;

	char **ptr = fp->path;

	while (*ptr != NULL) free(*ptr++);
	free(fp->path);

	return 0;
}

size_t fread(void *ptr, size_t bytes, FILE *fp) {
	if (bytes == 0) return 0;
	if (bytes > fp->size - fp->pointer) return 0;

	unsigned int disk = volumes[fp->volume].disk;
	unsigned int part = volumes[fp->volume].partition;

	switch (disktable[disk].parts[part].fs) {
		case fat32:
			return fat_read(ptr, bytes, fp);
			break;
		default:
			return 0;
			break;
	}
	return 0;
}

int fseek(FILE *fp, long long offset, fpos_t position) {
	switch (position) {
		case SEEK_SET:
			if (offset >= fp->size) return -2;
			if (offset < 0) return -2;
			fp->pointer = offset;
			break;
		case SEEK_CUR:
			if (offset + fp->pointer >= fp->size) return -2;
			if (offset + fp->pointer < 0) return -2;
			fp->pointer += offset;
			break;
		case SEEK_END:
			if (offset >= 0) return -2;
			if (-offset > fp->size) return -2;
			fp->pointer = fp->size + offset;
		default: return -1;
	}
	return 0;
}

void rewind(FILE *fp) {
	if (aligncheck(fp)) return;
	fp->pointer = 0;
}

unsigned long long fsize(FILE *fp) {
	if (aligncheck(fp)) return 0;
	return fp->size;
}
