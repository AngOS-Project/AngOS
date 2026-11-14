#ifndef ANGOS_TEST_STDIO_H
#define ANGOS_TEST_STDIO_H
#include <types.h>
#include <stddef.h>
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define FOPEN_READ 0x01U
#define FOPEN_WRITE 0x02U
#define FOPEN_CREATE 0x04U
#define FOPEN_TRUNCATE 0x08U
#define FOPEN_APPEND 0x10U
#define DIR_FS_DATA_WORDS 32
#define DIRENT_FILE 1
#define DIRENT_DIR 2
#define DIR_STATE_NATIVE 1U
#define DIR_STATE_ERROR 2U
typedef long long fpos_t;
typedef struct FILE {
    u8 used;
    char **path;
    u32 path_count;
    size_t pointer;
    u8 volume;
    unsigned long long size;
    u32 native_first_cluster;
    u8 native_attributes;
    u8 native;
    u8 mode;
    u8 write_pending;
    u8 native_reserved;
    u8 *write_buffer;
    u32 write_capacity;
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
int fabort(FILE *fp);
size_t fread(void *ptr, size_t bytes, FILE *fp);
size_t fwrite(const void *ptr, size_t bytes, FILE *fp);
int fseek(FILE *fp, long long offset, fpos_t position);
void rewind(FILE *fp);
unsigned long long fsize(FILE *fp);
DIR *opendir(char *path);
dirent *readdir(DIR *dir);
int closedir(DIR *dir);
int printf(const char *format, ...);
#endif
