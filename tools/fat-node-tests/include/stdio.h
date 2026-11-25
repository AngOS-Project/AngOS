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
#define FREMOVE_OK 1
#define FREMOVE_NOT_FOUND 0
#define FREMOVE_INVALID -1
#define FREMOVE_IO_ERROR -2
#define FREMOVE_READ_ONLY -3
#define FREMOVE_CLEANUP_ERROR -4
#define FREMOVE_NOT_FILE -5
#define FREMOVE_BUSY -6

#define FREMOVEDIRECTORY_OK 1
#define FREMOVEDIRECTORY_NOT_FOUND 0
#define FREMOVEDIRECTORY_INVALID -1
#define FREMOVEDIRECTORY_IO_ERROR -2
#define FREMOVEDIRECTORY_READ_ONLY -3
#define FREMOVEDIRECTORY_CLEANUP_ERROR -4
#define FREMOVEDIRECTORY_NOT_DIR -5
#define FREMOVEDIRECTORY_NOT_EMPTY -6
#define FREMOVEDIRECTORY_BUSY -7
#define FNEWDIRECTORY_OK 1
#define FNEWDIRECTORY_NO_SPACE 0
#define FNEWDIRECTORY_INVALID -1
#define FNEWDIRECTORY_IO_ERROR -2
#define FNEWDIRECTORY_EXISTS -3

#define FRENAME_OK 1
#define FRENAME_NOT_FOUND 0
#define FRENAME_INVALID -1
#define FRENAME_IO_ERROR -2
#define FRENAME_READ_ONLY -3
#define FRENAME_EXISTS -4
#define FRENAME_BUSY -5

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
int fremove(char *path);
int fremovedirectory(char *path);
int fnewdirectory(char *path);

int frename(char *path, char *new_name);

DIR *opendir(char *path);
dirent *readdir(DIR *dir);
int closedir(DIR *dir);
int printf(const char *format, ...);
#endif
