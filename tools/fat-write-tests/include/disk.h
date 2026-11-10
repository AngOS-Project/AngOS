#ifndef ANGOS_FAT_WRITE_TEST_DISK_H
#define ANGOS_FAT_WRITE_TEST_DISK_H
#include <types.h>
#define SECTOR_SIZE 512
#define LETTERS 26
typedef enum { none, fat12, fat16, fat32 } FS;
typedef struct { u64 loc; u64 size; FS fs; void *fsdata; } partinfo;
typedef struct { u64 blocks; partinfo parts[4]; } diskdata;
typedef struct { unsigned int disk; unsigned int partition; } volume_t;
extern diskdata *disktable;
extern volume_t *volumes;
bool disk_read_sector(u64 lba, void *buffer, unsigned int disk);
bool disk_write_sector(u64 lba, const void *buffer, unsigned int disk);
#endif
