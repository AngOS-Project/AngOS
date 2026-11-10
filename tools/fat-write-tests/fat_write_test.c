#include <types.h>
#include <disk.h>
#include <stdio.h>
#include <string.h>
#include "FATNode.h"
#include "FATWrite.h"

#define IMAGE_SECTORS 512
#define MAX_CLUSTERS 128
#define FAT_WRITE_TEST_ENTRIES 16

static u8 image[IMAGE_SECTORS][SECTOR_SIZE];
static u32 links[MAX_CLUSTERS];
static fat_node_fs filesystem;
static diskdata disk_table[4];
static volume_t volume_table[LETTERS];
diskdata *disktable = disk_table;
volume_t *volumes = volume_table;
static int failures;

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

static u32 eoc_value(void) {
    if (filesystem.volume.fat_bits == FAT_NATIVE_FAT12)
        return 0x0FFF;
    if (filesystem.volume.fat_bits == FAT_NATIVE_FAT16)
        return 0xFFFF;
    return 0x0FFFFFFF;
}

bool fat_native_fat_read(const fat_native_volume *volume, u32 cluster, u32 *value) {
    if (!volume || !value || cluster < 2 || cluster >= MAX_CLUSTERS)
        return false;
    *value = links[cluster];
    return true;
}

bool fat_native_fat_write(const fat_native_volume *volume, u32 cluster, u32 value) {
    if (!volume || cluster < 2 || cluster >= MAX_CLUSTERS)
        return false;
    links[cluster] = value;
    return true;
}

int fat_native_chain_next(const fat_native_volume *volume, u32 cluster, u32 *next_cluster) {
    if (!volume || !next_cluster || cluster < 2 || cluster >= MAX_CLUSTERS)
        return FAT_NATIVE_CHAIN_ERROR;
    u32 value = links[cluster];
    if (value >= (volume->fat_bits == FAT_NATIVE_FAT12 ? 0x0FF8U : volume->fat_bits == FAT_NATIVE_FAT16 ? 0xFFF8U : 0x0FFFFFF8U)) {
        *next_cluster = 0;
        return FAT_NATIVE_CHAIN_END;
    }
    if (value < 2 || value >= MAX_CLUSTERS)
        return FAT_NATIVE_CHAIN_ERROR;
    *next_cluster = value;
    return FAT_NATIVE_CHAIN_NEXT;
}

bool fat_native_cluster_sector(const fat_native_volume *volume, u32 cluster, u8 sector_index, u64 *lba) {
    if (!volume || !lba || cluster < 2 || cluster > volume->cluster_count + 1 || sector_index >= volume->sectors_per_cluster)
        return false;
    *lba = volume->partition_lba + volume->data_start + (u64)(cluster - 2) * volume->sectors_per_cluster + sector_index;
    return *lba < IMAGE_SECTORS;
}

bool fat_native_allocate_chain(fat_native_volume *volume, u32 count, u32 *first_cluster) {
    if (!volume || !first_cluster)
        return false;
    *first_cluster = 0;
    if (count == 0)
        return true;
    u32 first = 0;
    u32 previous = 0;
    u32 candidate = volume->allocation_hint < 2 ? 2 : volume->allocation_hint;
    for (u32 allocated = 0; allocated < count; ++allocated) {
        bool found = false;
        for (u32 scan = 0; scan < volume->cluster_count; ++scan) {
            if (candidate > volume->cluster_count + 1)
                candidate = 2;
            if (links[candidate] == 0) {
                found = true;
                break;
            }
            ++candidate;
        }
        if (!found) {
            if (first)
                fat_native_release_chain(volume, first);
            return false;
        }
        links[candidate] = eoc_value();
        if (previous)
            links[previous] = candidate;
        else
            first = candidate;
        previous = candidate;
        ++candidate;
    }
    volume->allocation_hint = candidate;
    *first_cluster = first;
    return true;
}

bool fat_native_release_chain(const fat_native_volume *volume, u32 first_cluster) {
    if (!volume)
        return false;
    u32 current = first_cluster;
    for (u32 step = 0; current != 0 && step < volume->cluster_count; ++step) {
        u32 next = 0;
        int state = fat_native_chain_next(volume, current, &next);
        links[current] = 0;
        if (state == FAT_NATIVE_CHAIN_END)
            return true;
        if (state != FAT_NATIVE_CHAIN_NEXT)
            return false;
        current = next;
    }
    return current == 0;
}

bool fat_node_root(const fat_node_fs *fs, fat_node *node) {
    if (!fs || !node)
        return false;
    memset(node, 0, sizeof(*node));
    node->first_cluster = fs->volume.root_cluster;
    node->attributes = 0x10;
    node->is_root = 1;
    node->fixed_root = fs->volume.fat_bits == FAT_NATIVE_FAT32 ? 0 : 1;
    return true;
}

bool fat_node_lookup(const fat_node_fs *fs, const char *path, fat_node *node) {
    if (!fs || !path || !node)
        return false;
    while (*path == '/' || *path == '\\')
        ++path;
    if (strcmp(path, "SUBDIR") != 0)
        return false;
    memset(node, 0, sizeof(*node));
    node->first_cluster = 10;
    node->attributes = 0x10;
    return true;
}

static void check(bool condition, const char *name) {
    if (condition)
        printf("PASS %s\n", name);
    else {
        printf("FAIL %s\n", name);
        ++failures;
    }
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

static u32 get32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void raw_entry(u8 *entry, const char name[11], u8 attributes, u32 cluster, u32 size) {
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = attributes;
    put16(entry + 26, (u16)cluster);
    put16(entry + 20, (u16)(cluster >> 16));
    put32(entry + 28, size);
}

static u64 root_lba(void) {
    if (filesystem.volume.fat_bits == FAT_NATIVE_FAT32)
        return filesystem.volume.partition_lba + filesystem.volume.data_start + (filesystem.volume.root_cluster - 2U);
    return filesystem.volume.partition_lba + filesystem.volume.root_start;
}

static u64 cluster_lba(u32 cluster) {
    return filesystem.volume.partition_lba + filesystem.volume.data_start + (cluster - 2U) * filesystem.volume.sectors_per_cluster;
}

static u8 *find_entry(u64 lba, const char name[11]) {
    for (u32 i = 0; i < FAT_WRITE_TEST_ENTRIES; ++i) {
        u8 *entry = &image[lba][i * 32U];
        if (entry[0] == 0)
            return 0;
        if (entry[0] != 0xE5 && memcmp(entry, name, 11) == 0)
            return entry;
    }
    return 0;
}

static void prepare(u8 bits) {
    memset(image, 0, sizeof(image));
    memset(links, 0, sizeof(links));
    memset(disk_table, 0, sizeof(disk_table));
    memset(volume_table, 0, sizeof(volume_table));
    memset(&filesystem, 0, sizeof(filesystem));
    filesystem.root_entries = 64;
    filesystem.volume.disk = 0;
    filesystem.volume.partition = 0;
    filesystem.volume.partition_lba = 0;
    filesystem.volume.partition_sectors = IMAGE_SECTORS;
    filesystem.volume.volume_sectors = IMAGE_SECTORS;
    filesystem.volume.root_start = 1;
    filesystem.volume.root_sectors = 4;
    filesystem.volume.data_start = 5;
    filesystem.volume.cluster_count = 100;
    filesystem.volume.root_cluster = bits == FAT_NATIVE_FAT32 ? 2 : 0;
    filesystem.volume.bytes_per_sector = 512;
    filesystem.volume.sectors_per_cluster = 1;
    filesystem.volume.fat_count = 2;
    filesystem.volume.fat_bits = bits;
    filesystem.volume.allocation_hint = 2;
    disk_table[0].parts[0].loc = 0;
    disk_table[0].parts[0].size = IMAGE_SECTORS;
    disk_table[0].parts[0].fs = bits == FAT_NATIVE_FAT12 ? fat12 : bits == FAT_NATIVE_FAT16 ? fat16 : fat32;
    volume_table[0].disk = 0;
    volume_table[0].partition = 0;
    if (bits == FAT_NATIVE_FAT32)
        links[2] = eoc_value();
    links[10] = eoc_value();
    raw_entry(&image[root_lba()][0], "SUBDIR     ", 0x10, 10, 0);
    memset(&image[root_lba()][32], 0, 32);
    raw_entry(&image[root_lba()][64], "STALE   TXT", 0x20, 0, 0);
    memset(image[cluster_lba(10)], 0, SECTOR_SIZE);
}

static void run_variant(u8 bits) {
    prepare(bits);
    u8 original[1300];
    for (u32 i = 0; i < sizeof(original); ++i)
        original[i] = (u8)((i * 7U) % 251U);
    int result = fat_node_write_file(&filesystem, "ALPHA.TXT", original, sizeof(original));
    check(result == FAT_WRITE_OK, bits == 12 ? "FAT12 create file" : bits == 16 ? "FAT16 create file" : "FAT32 create file");
    u8 *entry = find_entry(root_lba(), "ALPHA   TXT");
    check(entry && get32(entry + 28) == sizeof(original), "directory entry created with correct size");
    check(image[root_lba()][64] == 0, "directory end marker retained after insertion");
    u32 first = 0;
    if (entry)
        first = (u32)entry[26] | ((u32)entry[27] << 8) | ((u32)entry[20] << 16) | ((u32)entry[21] << 24);
    bool bytes_ok = entry != 0;
    u32 cluster = first;
    u32 copied = 0;
    while (bytes_ok && copied < sizeof(original)) {
        u64 lba = cluster_lba(cluster);
        u32 amount = sizeof(original) - copied > SECTOR_SIZE ? SECTOR_SIZE : sizeof(original) - copied;
        if (memcmp(image[lba], original + copied, amount) != 0)
            bytes_ok = false;
        copied += amount;
        if (copied < sizeof(original)) {
            u32 next = 0;
            if (fat_native_chain_next(&filesystem.volume, cluster, &next) != FAT_NATIVE_CHAIN_NEXT)
                bytes_ok = false;
            cluster = next;
        }
    }
    check(bytes_ok, "data written across allocated clusters");
    u8 replacement[17];
    memcpy(replacement, "replacement-data", sizeof(replacement));
    result = fat_node_write_file(&filesystem, "ALPHA.TXT", replacement, sizeof(replacement));
    check(result == FAT_WRITE_OK, "existing file replaced");
    entry = find_entry(root_lba(), "ALPHA   TXT");
    u32 replacement_cluster = entry ? (u32)entry[26] | ((u32)entry[27] << 8) | ((u32)entry[20] << 16) | ((u32)entry[21] << 24) : 0;
    check(entry && get32(entry + 28) == sizeof(replacement), "replacement size committed");
    check(replacement_cluster != first && links[first] == 0, "old cluster chain released after replacement");
    check(entry && memcmp(image[cluster_lba(replacement_cluster)], replacement, sizeof(replacement)) == 0, "replacement data verified");
    result = fat_node_write_file(&filesystem, "EMPTY.TXT", 0, 0);
    check(result == FAT_WRITE_OK, "zero-length file created");
    entry = find_entry(root_lba(), "EMPTY   TXT");
    check(entry && get32(entry + 28) == 0 && entry[26] == 0 && entry[27] == 0, "empty file has no cluster chain");
    raw_entry(&image[root_lba()][96], "LOCKED  TXT", 0x21, 0, 0);
    check(fat_node_write_file(&filesystem, "LOCKED.TXT", replacement, sizeof(replacement)) == FAT_WRITE_READ_ONLY, "read-only file replacement rejected");
    u8 nested[700];
    memset(nested, 0x5A, sizeof(nested));
    result = fat_node_write_file(&filesystem, "SUBDIR/INNER.BIN", nested, sizeof(nested));
    check(result == FAT_WRITE_OK, "nested directory file created");
    entry = find_entry(cluster_lba(10), "INNER   BIN");
    check(entry && get32(entry + 28) == sizeof(nested), "nested directory entry committed");
    raw_entry(&image[cluster_lba(10)][0], "ITEM0001TXT", 0x20, 0, 0);
    for (u32 i = 1; i < 16; ++i) {
        char name[11] = {'I','T','E','M','0','0','0','0','T','X','T'};
        name[7] = (char)('1' + (i % 8));
        raw_entry(&image[cluster_lba(10)][i * 32U], name, 0x20, 0, 0);
    }
    u8 extra = 0xA5;
    result = fat_node_write_file(&filesystem, "SUBDIR/EXTRA.TXT", &extra, 1);
    check(result == FAT_WRITE_OK, "full directory grows by one cluster");
    u32 grown = 0;
    check(fat_native_chain_next(&filesystem.volume, 10, &grown) == FAT_NATIVE_CHAIN_NEXT, "directory extension linked into chain");
    entry = grown ? find_entry(cluster_lba(grown), "EXTRA   TXT") : 0;
    u32 extra_cluster = entry ? (u32)entry[26] | ((u32)entry[27] << 8) : 0;
    check(entry && get32(entry + 28) == 1 && extra_cluster >= 2 && image[cluster_lba(extra_cluster)][0] == 0xA5, "new directory cluster contains file and data");
    check(image[cluster_lba(grown)][32] == 0, "new directory cluster has an end marker");
    result = fat_node_write_file(&filesystem, "TOOLONGNAME.TXT", &extra, 1);
    check(result == FAT_WRITE_INVALID, "unsupported long short-name component rejected");
}

int main(void) {
    run_variant(FAT_NATIVE_FAT12);
    run_variant(FAT_NATIVE_FAT16);
    run_variant(FAT_NATIVE_FAT32);
    printf("%s: %d failed checks\n", failures ? "FAILED FAT WRITE TESTS" : "ALL FAT WRITE TESTS PASSED", failures);
    return failures ? 1 : 0;
}
