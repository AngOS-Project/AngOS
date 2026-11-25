#ifndef ANGOS_FAT_WRITE_H
#define ANGOS_FAT_WRITE_H

#include "FATNode.h"

#define FAT_WRITE_OK 1
#define FAT_WRITE_NO_SPACE 0
#define FAT_WRITE_INVALID -1
#define FAT_WRITE_IO_ERROR -2
#define FAT_WRITE_READ_ONLY -3
#define FAT_WRITE_CLEANUP_ERROR -4

#define FAT_MKDIR_OK 1
#define FAT_MKDIR_NO_SPACE 0
#define FAT_MKDIR_INVALID -1
#define FAT_MKDIR_IO_ERROR -2
#define FAT_MKDIR_EXISTS -3

#define FAT_DELETE_OK 1
#define FAT_DELETE_NOT_FOUND 0
#define FAT_DELETE_INVALID -1
#define FAT_DELETE_IO_ERROR -2
#define FAT_DELETE_READ_ONLY -3
#define FAT_DELETE_CLEANUP_ERROR -4
#define FAT_DELETE_NOT_FILE -5

#define FAT_RMDIR_OK 1
#define FAT_RMDIR_NOT_FOUND 0
#define FAT_RMDIR_INVALID -1
#define FAT_RMDIR_IO_ERROR -2
#define FAT_RMDIR_READ_ONLY -3
#define FAT_RMDIR_CLEANUP_ERROR -4
#define FAT_RMDIR_NOT_DIR -5
#define FAT_RMDIR_NOT_EMPTY -6

int fat_node_write_file(fat_node_fs *filesystem, const char *path, const void *data, u32 length);
int fat_node_create_directory(fat_node_fs *filesystem, const char *path);
int fat_node_delete_file(fat_node_fs *filesystem, const char *path);
int fat_node_delete_directory(fat_node_fs *filesystem, const char *path);

#endif
