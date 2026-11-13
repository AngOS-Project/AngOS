#include <types.h>
#include <disk.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <FS.h>
#include "FATCore.h"

#define IMAGE_SECTORS 128
#define MAX_CLUSTERS 128

static u8 image[IMAGE_SECTORS][SECTOR_SIZE];
static u32 links[MAX_CLUSTERS];
static fat_native_volume configured_volume;
static diskdata disk_table[4];
static volume_t volume_table[LETTERS];
diskdata *disktable = disk_table;
volume_t *volumes = volume_table;

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
    if (next == 0 || next >= 0xFFF8U) {
        *next_cluster = 0;
        return FAT_NATIVE_CHAIN_END;
    }
    if (next < 2 || next > volume->cluster_count + 1)
        return FAT_NATIVE_CHAIN_ERROR;
    *next_cluster = next;
    return FAT_NATIVE_CHAIN_NEXT;
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

bool fat_native_allocate_chain(fat_native_volume *volume, u32 count, u32 *first_cluster) {
    if (!volume || !first_cluster || count == 0 || count > volume->cluster_count)
        return false;
    u32 first = 0;
    u32 previous = 0;
    u32 allocated = 0;
    for (u32 cluster = 2; cluster <= volume->cluster_count + 1 && allocated < count; ++cluster) {
        if (cluster >= MAX_CLUSTERS)
            break;
        if (links[cluster] != 0)
            continue;
        if (first == 0)
            first = cluster;
        if (previous != 0)
            links[previous] = cluster;
        previous = cluster;
        ++allocated;
    }
    if (allocated != count) {
        u32 current = first;
        for (u32 i = 0; i < allocated; ++i) {
            u32 next = links[current];
            links[current] = 0;
            current = next;
        }
        return false;
    }
    links[previous] = 0xFFFFU;
    *first_cluster = first;
    return true;
}

bool fat_native_release_chain(const fat_native_volume *volume, u32 first_cluster) {
    if (!volume || first_cluster < 2 || first_cluster > volume->cluster_count + 1)
        return false;
    u32 current = first_cluster;
    for (u32 step = 0; step < volume->cluster_count; ++step) {
        if (current < 2 || current >= MAX_CLUSTERS)
            return false;
        u32 next = links[current];
        links[current] = 0;
        if (next == 0 || next >= 0xFFF8U)
            return true;
        if (next < 2 || next > volume->cluster_count + 1)
            return false;
        current = next;
    }
    return false;
}

int fat16_dir_read(DIR *dir) {
    (void)dir;
    return 0;
}

int fat32_dir_read(DIR *dir) {
    (void)dir;
    return 0;
}

void fat32_dir_close(DIR *dir) {
    (void)dir;
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

static void short_entry(u8 *entry, const char name[11], u8 attributes, u32 cluster, u32 size) {
    memset(entry, 0, 32);
    memcpy(entry, name, 11);
    entry[11] = attributes;
    put16(entry + 26, (u16)cluster);
    put32(entry + 28, size);
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

static void prepare(void) {
    memset(image, 0, sizeof(image));
    memset(links, 0, sizeof(links));
    memset(disk_table, 0, sizeof(disk_table));
    memset(volume_table, 0, sizeof(volume_table));
    memset(&configured_volume, 0, sizeof(configured_volume));
    configured_volume.disk = 0;
    configured_volume.partition = 0;
    configured_volume.partition_lba = 1;
    configured_volume.partition_sectors = IMAGE_SECTORS - 1;
    configured_volume.volume_sectors = IMAGE_SECTORS - 1;
    configured_volume.fat_start = 1;
    configured_volume.fat_sectors = 4;
    configured_volume.root_start = 5;
    configured_volume.root_sectors = 2;
    configured_volume.data_start = 7;
    configured_volume.cluster_count = 100;
    configured_volume.bytes_per_sector = SECTOR_SIZE;
    configured_volume.sectors_per_cluster = 1;
    configured_volume.fat_count = 2;
    configured_volume.fat_bits = FAT_NATIVE_FAT16;
    disk_table[0].parts[0].loc = 1;
    disk_table[0].parts[0].size = IMAGE_SECTORS - 1;
    disk_table[0].parts[0].fs = fat16;
    disk_table[0].blocks = IMAGE_SECTORS;
    volume_table[0].disk = 0;
    volume_table[0].partition = 0;
    image[1][510] = 0x55;
    image[1][511] = 0xAA;
    put16(&image[1][17], 32);
    short_entry(&image[6][0], "DOC     TXT", 0x20, 5, 5);
    short_entry(&image[6][32], "FOLDER     ", 0x10, 8, 0);
    memset(&image[6][64], 0, 32);
    short_entry(&image[14][0], "INNER   TXT", 0x20, 10, 3);
    memset(&image[14][32], 0, 32);
    memcpy(image[11], "hello", 5);
    memcpy(image[16], "abc", 3);
    links[5] = 0xFFFFU;
    links[8] = 0xFFFFU;
    links[10] = 0xFFFFU;
}

int main(void) {
    prepare();
    FILE *file = fopen("|A/DOC.TXT", 0);
    check(file != 0 && fsize(file) == 5, "fopen and fsize through native FAT layer");
    char data[8] = {0};
    check(fread(data, 2, file) == 2 && memcmp(data, "he", 2) == 0, "fread initial segment");
    check(fseek(file, 1, SEEK_SET) == 0, "fseek absolute position");
    memset(data, 0, sizeof(data));
    check(fread(data, 2, file) == 2 && memcmp(data, "el", 2) == 0, "fread after absolute seek");
    check(fseek(file, -2, SEEK_END) == 0, "fseek from end");
    memset(data, 0, sizeof(data));
    check(fread(data, 2, file) == 2 && memcmp(data, "lo", 2) == 0, "fread final segment");
    rewind(file);
    memset(data, 0, sizeof(data));
    check(fread(data, 5, file) == 5 && memcmp(data, "hello", 5) == 0, "rewind and complete file read");
    check(fseek(file, 1, SEEK_END) != 0, "out-of-range seek rejected");
    check(fclose(file) == 0, "fclose releases file slot");
    check(fopen("|A/MISSING.TXT", 0) == 0, "missing file rejected");
    check(fopen("|A/FOLDER", 0) == 0, "directory rejected by fopen");
    file = fopen("|A/FOLDER/INNER.TXT", 0);
    memset(data, 0, sizeof(data));
    check(file && fread(data, 3, file) == 3 && memcmp(data, "abc", 3) == 0, "nested file lookup and read");
    check(file && fclose(file) == 0, "nested file close");
    DIR *directory = opendir("|A/");
    check(directory != 0, "root opendir");
    dirent *entry = directory ? readdir(directory) : 0;
    check(entry && strcmp(entry->name, "DOC.TXT") == 0 && entry->type == DIRENT_FILE, "root readdir file");
    entry = directory ? readdir(directory) : 0;
    check(entry && strcmp(entry->name, "FOLDER") == 0 && entry->type == DIRENT_DIR, "root readdir directory");
    check(directory && readdir(directory) == 0, "root readdir end");
    check(directory && closedir(directory) == 0, "closedir releases iterator");
    check(directory && closedir(directory) != 0, "double closedir rejected");
    directory = opendir("|A/FOLDER");
    entry = directory ? readdir(directory) : 0;
    check(entry && strcmp(entry->name, "INNER.TXT") == 0, "nested directory enumeration");
    check(directory && closedir(directory) == 0, "nested directory close");
    file = fopen("|A/NEW.TXT", FOPEN_WRITE | FOPEN_CREATE);
    check(file != 0, "fopen creates a new file for writing");
    check(file && fwrite("new-data", 8, file) == 8, "fwrite writes new file contents");
    check(file && fclose(file) == 0, "fclose commits new file");
    file = fopen("|A/NEW.TXT", 0);
    memset(data, 0, sizeof(data));
    check(file && fsize(file) == 8 && fread(data, 8, file) == 8 && memcmp(data, "new-data", 8) == 0, "created file can be reopened and read");
    check(file && fclose(file) == 0, "created file read handle closes");
    file = fopen("|A/DOC.TXT", FOPEN_WRITE | FOPEN_APPEND);
    check(file != 0, "fopen opens existing file in append mode");
    check(file && fwrite("!", 1, file) == 1, "fwrite appends to existing contents");
    check(file && fclose(file) == 0, "fclose commits appended contents");
    file = fopen("|A/DOC.TXT", 0);
    memset(data, 0, sizeof(data));
    check(file && fsize(file) == 6 && fread(data, 6, file) == 6 && memcmp(data, "hello!", 6) == 0, "append preserves old data and adds new data");
    check(file && fclose(file) == 0, "appended file read handle closes");
    file = fopen("|A/DOC.TXT", FOPEN_WRITE | FOPEN_TRUNCATE);
    check(file && fsize(file) == 0, "truncate mode starts with an empty file");
    check(file && fwrite("replacement", 11, file) == 11, "fwrite replaces file contents");
    check(file && fclose(file) == 0, "fclose commits replacement contents");
    file = fopen("|A/DOC.TXT", 0);
    memset(data, 0, sizeof(data));
    check(file && fsize(file) == 11 && fread(data, 11, file) == 11 && memcmp(data, "replacement", 11) == 0, "replaced file can be reopened and read");
    check(links[5] == 0, "replacement releases the previous cluster chain");
    if (file) {
        printf("DEBUG: before final fclose\n");
        int close_result = fclose(file);
        printf("DEBUG: after final fclose, result=%d\n", close_result);
        check(close_result == 0, "replaced file read handle closes");
    } else {
        check(false, "replaced file read handle closes");
    }
    printf("%s: %d failed checks\n", failures ? "FAILED" : "ALL FS API TESTS PASSED", failures);
    return failures ? 1 : 0;
}
