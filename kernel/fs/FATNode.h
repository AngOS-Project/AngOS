#ifndef ANGOS_FAT_NODE_H
#define ANGOS_FAT_NODE_H

#include <types.h>
#include "FATCore.h"

#define FAT_NODE_NAME_MAX 255
#define FAT_NODE_FILE 1
#define FAT_NODE_DIRECTORY 2
#define FAT_NODE_ERROR -1
#define FAT_NODE_END 0
#define FAT_NODE_ENTRY 1

typedef struct {
    u32 first_cluster;
    u32 size;
    u8 attributes;
    u8 is_root;
    u8 fixed_root;
    u8 reserved;
} fat_node;

typedef struct {
    fat_native_volume volume;
    u32 root_entries;
} fat_node_fs;

typedef struct {
    const fat_node_fs *filesystem;
    fat_node directory;
    u32 current_cluster;
    u32 cluster_steps;
    u32 entry_index;
    u8 sector_index;
    u16 entry_offset;
    u8 buffer_valid;
    u8 ended;
    u8 lfn_active;
    u8 lfn_expected;
    u8 lfn_checksum;
    u16 lfn_units[260];
    u8 sector[512];
} fat_node_dir;

typedef struct {
    fat_node node;
    char name[256];
    u8 type;
} fat_node_entry;

bool fat_node_mount(fat_node_fs *filesystem, unsigned int disk, unsigned int partition);
bool fat_node_root(const fat_node_fs *filesystem, fat_node *node);
bool fat_node_lookup(const fat_node_fs *filesystem, const char *path, fat_node *node);
bool fat_node_read(const fat_node_fs *filesystem, const fat_node *node, u64 offset, void *buffer, size_t count, size_t *read_count);
bool fat_node_dir_open(const fat_node_fs *filesystem, const fat_node *node, fat_node_dir *directory);
int fat_node_dir_next(fat_node_dir *directory, fat_node_entry *entry);

#endif
