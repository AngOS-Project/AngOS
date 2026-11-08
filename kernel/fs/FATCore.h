#ifndef ANGOS_FAT_CORE_H
#define ANGOS_FAT_CORE_H

#include <types.h>
#include <disk.h>

#define FAT_NATIVE_FAT12 12
#define FAT_NATIVE_FAT16 16
#define FAT_NATIVE_FAT32 32

#define FAT_NATIVE_CHAIN_ERROR -1
#define FAT_NATIVE_CHAIN_END 0
#define FAT_NATIVE_CHAIN_NEXT 1

typedef struct {
    unsigned int disk;
    unsigned int partition;
    u64 partition_lba;
    u64 partition_sectors;
    u32 volume_sectors;
    u32 fat_start;
    u32 fat_sectors;
    u32 root_start;
    u32 root_sectors;
    u32 data_start;
    u32 cluster_count;
    u32 root_cluster;
    u32 fsinfo_sector;
    u32 allocation_hint;
    u16 bytes_per_sector;
    u8 sectors_per_cluster;
    u8 fat_count;
    u8 fat_bits;
    u8 mirrored;
    u8 active_fat;
} fat_native_volume;

bool fat_native_mount(fat_native_volume *volume, unsigned int disk, unsigned int partition);
bool fat_native_cluster_sector(const fat_native_volume *volume, u32 cluster, u8 sector_index, u64 *lba);
bool fat_native_fat_read(const fat_native_volume *volume, u32 cluster, u32 *value);
bool fat_native_fat_write(const fat_native_volume *volume, u32 cluster, u32 value);
int fat_native_chain_next(const fat_native_volume *volume, u32 cluster, u32 *next_cluster);
bool fat_native_find_free(fat_native_volume *volume, u32 *cluster);
bool fat_native_allocate_chain(fat_native_volume *volume, u32 count, u32 *first_cluster);
bool fat_native_release_chain(const fat_native_volume *volume, u32 first_cluster);

#endif
