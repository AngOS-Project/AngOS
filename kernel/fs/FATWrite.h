#ifndef ANGOS_FAT_WRITE_H
#define ANGOS_FAT_WRITE_H

#include "FATNode.h"

#define FAT_WRITE_OK 1
#define FAT_WRITE_NO_SPACE 0
#define FAT_WRITE_INVALID -1
#define FAT_WRITE_IO_ERROR -2
#define FAT_WRITE_READ_ONLY -3
#define FAT_WRITE_CLEANUP_ERROR -4

#define FAT_NEWDIRECTORY_OK 1
#define FAT_NEWDIRECTORY_NO_SPACE 0
#define FAT_NEWDIRECTORY_INVALID -1
#define FAT_NEWDIRECTORY_IO_ERROR -2
#define FAT_NEWDIRECTORY_EXISTS -3

#define FAT_DELETE_OK 1
#define FAT_DELETE_NOT_FOUND 0
#define FAT_DELETE_INVALID -1
#define FAT_DELETE_IO_ERROR -2
#define FAT_DELETE_READ_ONLY -3
#define FAT_DELETE_CLEANUP_ERROR -4
#define FAT_DELETE_NOT_FILE -5

#define FAT_REMOVEDIRECTORY_OK 1
#define FAT_REMOVEDIRECTORY_NOT_FOUND 0
#define FAT_REMOVEDIRECTORY_INVALID -1
#define FAT_REMOVEDIRECTORY_IO_ERROR -2
#define FAT_REMOVEDIRECTORY_READ_ONLY -3
#define FAT_REMOVEDIRECTORY_CLEANUP_ERROR -4
#define FAT_REMOVEDIRECTORY_NOT_DIR -5
#define FAT_REMOVEDIRECTORY_NOT_EMPTY -6

#define FAT_RENAME_OK 1
#define FAT_RENAME_NOT_FOUND 0
#define FAT_RENAME_INVALID -1
#define FAT_RENAME_IO_ERROR -2
#define FAT_RENAME_READ_ONLY -3
#define FAT_RENAME_EXISTS -4

int fat_node_write_file(fat_node_fs *filesystem, const char *path, const void *data, u32 length);
int fat_node_new_directory(fat_node_fs *filesystem, const char *path);
int fat_node_delete_file(fat_node_fs *filesystem, const char *path);
int fat_node_remove_directory(fat_node_fs *filesystem, const char *path);
int fat_node_rename_entry(fat_node_fs *filesystem, const char *path, const char *new_name);

#endif
