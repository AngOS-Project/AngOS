#ifndef ANGOS_STDIO_H
#define ANGOS_STDIO_H

#include <types.h>
#include <stddef.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define DIR_FS_DATA_WORDS 32

#define DIRENT_FILE 1
#define DIRENT_DIR  2

typedef long long fpos_t;

typedef struct FILE {
    u8 used;
    char **path;
    size_t pointer;
    u8 volume;
    unsigned long long size;
} FILE;

typedef struct dirent {
    char name[256];
    u8 type;
    u32 size;
} dirent;

typedef struct DIR {
    u8 used;
    u8 volume;
    u8 fs;
    u8 reserved;
    u64 fsdata[DIR_FS_DATA_WORDS];
    dirent entry;
} DIR;

FILE *fopen(char *path, u8 flags);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t bytes, FILE *fp);
int fseek(FILE *fp, long long offset, fpos_t position);
void rewind(FILE *fp);
unsigned long long fsize(FILE *fp);

DIR *opendir(char *path);
dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#endif