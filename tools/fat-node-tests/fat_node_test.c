#include <types.h>
#include <disk.h>
#include <stdio.h>
#include <string.h>
#include "FATNode.h"

#define IMAGE_SECTORS 2048
#define MAX_CLUSTERS 256

static u8 image[IMAGE_SECTORS][SECTOR_SIZE];
static u32 links[MAX_CLUSTERS];
static fat_native_volume configured_volume;
static diskdata disk_table[4];
diskdata *disktable = disk_table;
volume_t *volumes;

bool disk_read_sector(u64 lba, void *buffer, unsigned int disk) {
    if (!buffer || disk != 0 || lba >= IMAGE_SECTORS)
        return false;
    memcpy(buffer, image[lba], SECTOR_SIZE);
    return true;
}

bool disk_write_sector(u64 lba, const void *buffer, unsigned int disk) {
    if (!buffer || disk != 0 || lba >= IMAGE_SECTORS)
        return false;
    memcpy(image[lba], buffer, SECTOR_SIZE);
    return true;
}

bool fat_native_mount(fat_native_volume *volume, unsigned int disk, unsigned int partition) {
    if (!volume || disk != 0 || partition != 0)
        return false;
    *volume = configured_volume;
    return true;
}

bool fat_native_cluster_sector(const fat_native_volume *volume, u32 cluster, u8 sector_index, u64 *lba) {
    if (!volume || !lba || cluster < 2 || cluster > volume->cluster_count + 1 || sector_index >= volume->sectors_per_cluster)
        return false;
    *lba = volume->partition_lba + volume->data_start + (u64)(cluster - 2) * volume->sectors_per_cluster + sector_index;
    return *lba < IMAGE_SECTORS;
}

int fat_native_chain_next(const fat_native_volume *volume, u32 cluster, u32 *next_cluster) {
    if (!volume || !next_cluster || cluster < 2 || cluster >= MAX_CLUSTERS)
        return FAT_NATIVE_CHAIN_ERROR;
    u32 next = links[cluster];
    if (next == 0) {
        *next_cluster = 0;
        return FAT_NATIVE_CHAIN_END;
    }
    if (next < 2 || next > volume->cluster_count + 1)
        return FAT_NATIVE_CHAIN_ERROR;
    *next_cluster = next;
    return FAT_NATIVE_CHAIN_NEXT;
}

static void put16(u8 *p, u16 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static void put32(u8 *p, u32 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static u8 checksum(const u8 *name) {
    u8 sum = 0;
    for (u32 i = 0; i < 11; ++i)
        sum = (u8)(((sum & 1U) ? 0x80U : 0U) + (sum >> 1) + name[i]);
    return sum;
}

static void short_entry(u8 *entry, const char name[11], u8 attributes, u32 cluster, u32 size, bool fat32) {
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = attributes;
    put16(entry + 26, (u16)cluster);
    if (fat32)
        put16(entry + 20, (u16)(cluster >> 16));
    put32(entry + 28, size);
}

static void lfn_entry(u8 *entry, u8 sequence, u8 sum, const u16 chars[13]) {
    static const u8 positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    memset(entry, 0xFF, 32);
    entry[0] = sequence;
    entry[11] = 0x0F;
    entry[12] = 0;
    entry[13] = sum;
    put16(entry + 26, 0);
    for (u32 i = 0; i < 13; ++i)
        put16(entry + positions[i], chars[i]);
}

static void write_entry(u64 lba, u32 index, const u8 *entry) {
    memcpy(&image[lba][index * 32], entry, 32);
}

static int failures;

static void check(bool condition, const char *name) {
    if (condition)
        printf("PASS %s\n", name);
    else {
        printf("FAIL %s\n", name);
        ++failures;
    }
}

static void prepare(u8 bits) {
    memset(image, 0, sizeof(image));
    memset(links, 0, sizeof(links));
    memset(disk_table, 0, sizeof(disk_table));
    memset(&configured_volume, 0, sizeof(configured_volume));
    configured_volume.disk = 0;
    configured_volume.partition = 0;
    configured_volume.partition_lba = 1;
    configured_volume.partition_sectors = 1000;
    configured_volume.volume_sectors = 1000;
    configured_volume.fat_start = 1;
    configured_volume.fat_sectors = 4;
    configured_volume.root_start = 5;
    configured_volume.root_sectors = bits == FAT_NATIVE_FAT32 ? 0 : 2;
    configured_volume.data_start = 7;
    configured_volume.cluster_count = 100;
    configured_volume.root_cluster = bits == FAT_NATIVE_FAT32 ? 2 : 0;
    configured_volume.bytes_per_sector = 512;
    configured_volume.sectors_per_cluster = 1;
    configured_volume.fat_count = 2;
    configured_volume.fat_bits = bits;
    disk_table[0].parts[0].loc = 1;
    disk_table[0].parts[0].size = 1000;
    disk_table[0].blocks = IMAGE_SECTORS;
    image[1][510] = 0x55;
    image[1][511] = 0xAA;
    put16(&image[1][17], bits == FAT_NATIVE_FAT32 ? 0 : 32);
    links[5] = 9;
    links[9] = 0;
    links[8] = 0;
    links[10] = 0;
    links[11] = 0;
    u64 root_lba = bits == FAT_NATIVE_FAT32 ? 1 + 7 : 1 + 5;
    u8 entry[32];
    u8 alias[11] = {'L','O','N','G','D','O','~','1','T','X','T'};
    u16 part1[13] = {'L','o','n','g',' ','D','o','c','u','m','e','n','t'};
    u16 part2[13] = {'.','t','x','t',0,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF};
    lfn_entry(entry, 0x42, checksum(alias), part2);
    write_entry(root_lba, 0, entry);
    lfn_entry(entry, 1, checksum(alias), part1);
    write_entry(root_lba, 1, entry);
    short_entry(entry, (const char *)alias, 0x20, 11, 6, bits == FAT_NATIVE_FAT32);
    write_entry(root_lba, 2, entry);
    short_entry(entry, "FRAG    BIN", 0x20, 5, 700, bits == FAT_NATIVE_FAT32);
    write_entry(root_lba, 3, entry);
    short_entry(entry, "EMPTY   TXT", 0x20, 0, 0, bits == FAT_NATIVE_FAT32);
    write_entry(root_lba, 4, entry);
    short_entry(entry, "SUBDIR     ", 0x10, 8, 0, bits == FAT_NATIVE_FAT32);
    write_entry(root_lba, 5, entry);
    memset(entry, 0, sizeof(entry));
    write_entry(root_lba, 6, entry);
    u64 subdir_lba = 1 + 7 + (8 - 2);
    short_entry(entry, "INNER   TXT", 0x20, 10, 3, bits == FAT_NATIVE_FAT32);
    write_entry(subdir_lba, 0, entry);
    memset(entry, 0, sizeof(entry));
    write_entry(subdir_lba, 1, entry);
    for (u32 i = 0; i < 512; ++i) {
        image[1 + 7 + (5 - 2)][i] = (u8)(i % 251U);
        image[1 + 7 + (9 - 2)][i] = (u8)((512U + i) % 251U);
    }
    memcpy(image[1 + 7 + (11 - 2)], "hello\n", 6);
    memcpy(image[1 + 7 + (10 - 2)], "abc", 3);
}

static void run_variant(u8 bits) {
    prepare(bits);
    fat_node_fs filesystem;
    fat_node root;
    fat_node target;
    fat_node_entry entry;
    fat_node_dir directory;
    bool mounted = fat_node_mount(&filesystem, 0, 0);
    check(mounted, bits == 12 ? "FAT12 mount" : bits == 16 ? "FAT16 mount" : "FAT32 mount");
    check(fat_node_root(&filesystem, &root), "root node open");
    check(fat_node_lookup(&filesystem, "FRAG.BIN", &target), "short-name path lookup");
    u8 data[200];
    size_t read_count = 0;
    bool read_ok = fat_node_read(&filesystem, &target, 510, data, sizeof(data), &read_count);
    bool data_ok = read_ok && read_count == 190;
    for (u32 i = 0; data_ok && i < read_count; ++i)
        if (data[i] != (u8)((510U + i) % 251U))
            data_ok = false;
    check(data_ok, "fragmented read across cluster boundary");
    check(fat_node_lookup(&filesystem, "Long Document.txt", &target), "long filename lookup");
    u8 named[8];
    read_count = 0;
    read_ok = fat_node_read(&filesystem, &target, 0, named, sizeof(named), &read_count);
    check(read_ok && read_count == 6 && memcmp(named, "hello\n", 6) == 0, "long filename file read");
    check(fat_node_lookup(&filesystem, "SUBDIR/INNER.TXT", &target), "nested path lookup");
    read_count = 0;
    read_ok = fat_node_read(&filesystem, &target, 0, named, sizeof(named), &read_count);
    check(read_ok && read_count == 3 && memcmp(named, "abc", 3) == 0, "nested file read");
    check(fat_node_lookup(&filesystem, "EMPTY.TXT", &target), "empty file lookup");
    read_count = 99;
    read_ok = fat_node_read(&filesystem, &target, 0, named, sizeof(named), &read_count);
    check(read_ok && read_count == 0, "empty file read");
    check(!fat_node_lookup(&filesystem, "MISSING.TXT", &target), "missing path rejected");
    check(fat_node_dir_open(&filesystem, &root, &directory), "root directory open");
    u32 names = 0;
    bool saw_lfn = false;
    int state;
    while ((state = fat_node_dir_next(&directory, &entry)) == FAT_NODE_ENTRY) {
        ++names;
        if (strcmp(entry.name, "Long Document.txt") == 0)
            saw_lfn = true;
    }
    check(state == FAT_NODE_END && names == 4, "root directory enumeration");
    check(saw_lfn, "long filename directory entry");
    if (bits == FAT_NATIVE_FAT12) {
        links[9] = 5;
        check(fat_node_lookup(&filesystem, "FRAG.BIN", &target), "cycle fixture file lookup");
        target.size = 1200;
        u8 all[1200];
        read_count = 0;
        read_ok = fat_node_read(&filesystem, &target, 0, all, sizeof(all), &read_count);
        check(!read_ok && read_count == 0, "cyclic file chain rejected before reading");
        links[9] = 0;
        target.size = 700;
    }
}

int main(void) {
    run_variant(FAT_NATIVE_FAT12);
    run_variant(FAT_NATIVE_FAT16);
    run_variant(FAT_NATIVE_FAT32);
    printf("%s: %d failed checks\n", failures ? "FAILED" : "ALL FAT NODE TESTS PASSED", failures);
    return failures ? 1 : 0;
}
