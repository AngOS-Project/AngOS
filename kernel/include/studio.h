#ifndef ANGOS_STDIO_H
#define ANGOS_STDIO_H

#include <types.h>
#include <stddef.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef long long fpos_t;

typedef struct FILE {
    u8 used;
    char **path;
    size_t pointer;
    u8 volume;
    unsigned long long size;
} FILE;

FILE *fopen(char *path, u8 flags);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t bytes, FILE *fp);
int fseek(FILE *fp, long long offset, fpos_t position);
void rewind(FILE *fp);
unsigned long long fsize(FILE *fp);

#endif
