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

bool fat_native_mount(fat_native_volume *volume, unsigned int disk, unsigned int partition) {
    (void)volume;
    (void)disk;
    (void)partition;
    return false;
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


static u8 short_checksum(const u8 name[11]) {
    u8 sum = 0;
    for (u32 i = 0; i < 11; ++i)
        sum = (u8)(((sum & 1U) ? 0x80U : 0U) + (sum >> 1) + name[i]);
    return sum;
}

static void raw_lfn_entry(u8 *entry, u8 sequence, u8 checksum, const u16 units[26]) {
    static const u8 positions[13] = {
        1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
    };
    memset(entry, 0xFF, 32);
    entry[0] = sequence;
    entry[11] = 0x0F;
    entry[12] = 0;
    entry[13] = checksum;
    entry[26] = 0;
    entry[27] = 0;
    u32 ordinal = sequence & 0x1FU;
    u32 base = (ordinal - 1U) * 13U;
    for (u32 i = 0; i < 13; ++i)
        put16(&entry[positions[i]], units[base + i]);
}

static void run_long_name_delete_variant(u8 bits) {
    prepare(bits);
    u64 first_lba = root_lba();
    u64 second_lba = bits == FAT_NATIVE_FAT32
        ? cluster_lba(3)
        : first_lba + 1U;

    if (bits == FAT_NATIVE_FAT32) {
        links[2] = 3;
        links[3] = eoc_value();
    }

    for (u32 i = 0; i < 15; ++i)
        image[first_lba][i * 32U] = 0xE5;

    char name[] = "Long Document.txt";
    u16 units[26];
    for (u32 i = 0; i < 26; ++i)
        units[i] = 0xFFFF;
    u32 length = 0;
    while (name[length]) {
        units[length] = (u8)name[length];
        ++length;
    }
    units[length] = 0;

    const u8 alias[11] = {
        'L', 'O', 'N', 'G', 'D', 'O', '~', '1', 'T', 'X', 'T'
    };
    u8 checksum = short_checksum(alias);
    raw_lfn_entry(&image[first_lba][15U * 32U], 0x42, checksum, units);
    raw_lfn_entry(&image[second_lba][0], 0x01, checksum, units);
    raw_entry(&image[second_lba][32], (const char *)alias, 0x20, 12, 12);
    links[12] = eoc_value();

    int result = fat_node_delete_file(&filesystem, "Long Document.txt");
    check(result == FAT_DELETE_OK,
        bits == 12 ? "FAT12 long filename deletion" :
        bits == 16 ? "FAT16 long filename deletion" :
        "FAT32 long filename deletion");
    check(
        image[first_lba][15U * 32U] == 0xE5 &&
        image[second_lba][0] == 0xE5 &&
        image[second_lba][32] == 0xE5,
        "long and short entries deleted across sector boundary"
    );
    check(links[12] == 0, "long filename data cluster released");
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

    u8 chain_data[1300];
    memset(chain_data, 0x6B, sizeof(chain_data));
    result = fat_node_write_file(&filesystem, "CHAIN.BIN", chain_data, sizeof(chain_data));
    entry = find_entry(root_lba(), "CHAIN   BIN");
    u32 delete_first_cluster = entry
        ? (u32)entry[26] | ((u32)entry[27] << 8) |
          ((u32)entry[20] << 16) | ((u32)entry[21] << 24)
        : 0;
    u32 delete_second_cluster = 0;
    u32 delete_third_cluster = 0;
    bool delete_chain_ready = result == FAT_WRITE_OK && entry &&
        fat_native_chain_next(&filesystem.volume, delete_first_cluster, &delete_second_cluster) == FAT_NATIVE_CHAIN_NEXT &&
        fat_native_chain_next(&filesystem.volume, delete_second_cluster, &delete_third_cluster) == FAT_NATIVE_CHAIN_NEXT;
    check(delete_chain_ready, "multi-cluster deletion fixture created");
    int delete_result = delete_chain_ready
        ? fat_node_delete_file(&filesystem, "CHAIN.BIN")
        : FAT_DELETE_IO_ERROR;
    check(delete_result == FAT_DELETE_OK, "multi-cluster file deleted");
    check(
        entry && entry[0] == 0xE5 &&
        links[delete_first_cluster] == 0 &&
        links[delete_second_cluster] == 0 &&
        links[delete_third_cluster] == 0,
        "multi-cluster chain fully released"
    );
    result = fat_node_write_file(&filesystem, "EMPTY.TXT", 0, 0);
    check(result == FAT_WRITE_OK, "zero-length file created");
    entry = find_entry(root_lba(), "EMPTY   TXT");
    check(entry && get32(entry + 28) == 0 && entry[26] == 0 && entry[27] == 0, "empty file has no cluster chain");
    check(
        fat_node_delete_file(&filesystem, "EMPTY.TXT") == FAT_DELETE_OK,
        "zero-length file deleted"
    );
    check(entry && entry[0] == 0xE5, "zero-length directory entry marked deleted");
    raw_entry(&image[root_lba()][96], "LOCKED  TXT", 0x21, 0, 0);
    check(fat_node_write_file(&filesystem, "LOCKED.TXT", replacement, sizeof(replacement)) == FAT_WRITE_READ_ONLY, "read-only file replacement rejected");
    check(
        fat_node_delete_file(&filesystem, "LOCKED.TXT") == FAT_DELETE_READ_ONLY,
        "read-only file deletion rejected"
    );
    check(image[root_lba()][96] == 'L', "read-only file entry remains present");
    u8 nested[700];
    memset(nested, 0x5A, sizeof(nested));
    result = fat_node_write_file(&filesystem, "SUBDIR/INNER.BIN", nested, sizeof(nested));
    check(result == FAT_WRITE_OK, "nested directory file created");
    entry = find_entry(cluster_lba(10), "INNER   BIN");
    check(entry && get32(entry + 28) == sizeof(nested), "nested directory entry committed");
    u32 inner_first_cluster = entry
        ? (u32)entry[26] | ((u32)entry[27] << 8) |
          ((u32)entry[20] << 16) | ((u32)entry[21] << 24)
        : 0;
    u32 inner_second_cluster = 0;
    bool inner_chain_ready = entry &&
        fat_native_chain_next(&filesystem.volume, inner_first_cluster, &inner_second_cluster) == FAT_NATIVE_CHAIN_NEXT;
    check(inner_chain_ready, "nested multi-cluster file chain created");
    check(
        fat_node_delete_file(&filesystem, "SUBDIR/INNER.BIN") == FAT_DELETE_OK,
        "nested file deleted"
    );
    check(
        entry && entry[0] == 0xE5 &&
        links[inner_first_cluster] == 0 &&
        links[inner_second_cluster] == 0,
        "nested entry removed and its chain released"
    );
    check(
        fat_node_delete_file(&filesystem, "SUBDIR") == FAT_DELETE_NOT_FILE,
        "directory rejected by file deletion API"
    );
    check(
        fat_node_delete_file(&filesystem, "MISSING.TXT") == FAT_DELETE_NOT_FOUND,
        "missing file deletion reported"
    );
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
    run_long_name_delete_variant(FAT_NATIVE_FAT12);
    run_long_name_delete_variant(FAT_NATIVE_FAT16);
    run_long_name_delete_variant(FAT_NATIVE_FAT32);
    printf("%s: %d failed checks\n", failures ? "FAILED FAT WRITE TESTS" : "ALL FAT WRITE TESTS PASSED", failures);
    return failures ? 1 : 0;
}
