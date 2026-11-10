#ifndef ANGOS_FAT_WRITE_H
#define ANGOS_FAT_WRITE_H

#include "FATNode.h"

#define FAT_WRITE_OK 1
#define FAT_WRITE_NO_SPACE 0
#define FAT_WRITE_INVALID -1
#define FAT_WRITE_IO_ERROR -2
#define FAT_WRITE_READ_ONLY -3
#define FAT_WRITE_CLEANUP_ERROR -4

int fat_node_write_file(fat_node_fs *filesystem, const char *path, const void *data, u32 length);

#endif
