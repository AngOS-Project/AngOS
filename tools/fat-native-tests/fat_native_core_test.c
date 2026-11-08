#include <types.h>
#include <disk.h>
#include "FATCore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SECTORS 80000

static u8 image[TEST_SECTORS][SECTOR_SIZE];
static diskdata disks[4];
diskdata *disktable = disks;
volume_t *volumes;

bool disk_read_sector(u64 lba, void *buffer, unsigned int disk) {
    if (!buffer || disk != 0 || lba >= TEST_SECTORS)
        return false;
    memcpy(buffer, image[lba], SECTOR_SIZE);
    return true;
}

bool disk_write_sector(u64 lba, const void *buffer, unsigned int disk) {
    if (!buffer || disk != 0 || lba >= TEST_SECTORS)
        return false;
    memcpy(image[lba], buffer, SECTOR_SIZE);
    return true;
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

static void make_volume(unsigned int partition, u32 total, u16 reserved, u8 fats, u32 fat_size, u16 root_entries, u8 spc, u8 bits) {
    u64 base = 100;
    u8 *boot = image[base];
    memset(boot, 0, SECTOR_SIZE);
    put16(&boot[11], 512);
    boot[13] = spc;
    put16(&boot[14], reserved);
    boot[16] = fats;
    put16(&boot[17], root_entries);
    if (total <= 65535)
        put16(&boot[19], (u16)total);
    else
        put32(&boot[32], total);
    if (bits == FAT_NATIVE_FAT32) {
        put32(&boot[36], fat_size);
        put32(&boot[44], 2);
        put16(&boot[48], 1);
    } else {
        put16(&boot[22], (u16)fat_size);
    }
    boot[510] = 0x55;
    boot[511] = 0xAA;
    disks[0].parts[partition].loc = base;
    disks[0].parts[partition].size = total;
    disks[0].blocks = TEST_SECTORS;
    memset(image[base + reserved], 0, (size_t)fats * fat_size * SECTOR_SIZE);
}

static void set_fat32_raw(u64 lba, u32 cluster, u32 value) {
    u8 *fat = image[lba + (cluster * 4) / 512];
    u32 offset = (cluster * 4) % 512;
    put32(&fat[offset], value);
}

static int require(bool condition, const char *name) {
    if (!condition) {
        printf("FAIL: %s\n", name);
        return 0;
    }
    printf("PASS: %s\n", name);
    return 1;
}

int main(void) {
    int passed = 1;
    memset(disks, 0, sizeof(disks));

    make_volume(0, 400, 1, 2, 2, 64, 1, FAT_NATIVE_FAT12);
    fat_native_volume fat12;
    passed &= require(fat_native_mount(&fat12, 0, 0) && fat12.fat_bits == FAT_NATIVE_FAT12, "FAT12 geometry mount");
    passed &= require(fat_native_fat_write(&fat12, 340, 0x123) && fat_native_fat_write(&fat12, 341, 0x456), "FAT12 packed entries across sector boundary write");
    u32 value = 0;
    passed &= require(fat_native_fat_read(&fat12, 340, &value) && value == 0x123, "FAT12 even entry neighbour preserved");
    passed &= require(fat_native_fat_read(&fat12, 341, &value) && value == 0x456, "FAT12 odd entry across sector boundary read");
    passed &= require(image[101][511] == image[103][511] && image[102][0] == image[104][0], "FAT12 mirrored boundary bytes");
    u32 first = 0;
    passed &= require(fat_native_allocate_chain(&fat12, 3, &first) && first >= 2, "FAT12 chain allocation");
    u32 next = 0;
    passed &= require(fat_native_chain_next(&fat12, first, &next) == FAT_NATIVE_CHAIN_NEXT, "FAT12 first chain link");
    passed &= require(fat_native_chain_next(&fat12, next, &value) == FAT_NATIVE_CHAIN_NEXT, "FAT12 second chain link");
    passed &= require(fat_native_chain_next(&fat12, value, &next) == FAT_NATIVE_CHAIN_END, "FAT12 end-of-chain marker");
    passed &= require(fat_native_release_chain(&fat12, first), "FAT12 chain release");
    passed &= require(fat_native_fat_write(&fat12, 50, 51) && fat_native_fat_write(&fat12, 51, 50), "FAT12 cycle fixture");
    passed &= require(!fat_native_release_chain(&fat12, 50), "FAT12 cyclic chain rejected before release");
    u32 loop_a = 0;
    u32 loop_b = 0;
    passed &= require(fat_native_fat_read(&fat12, 50, &loop_a) && loop_a == 51 && fat_native_fat_read(&fat12, 51, &loop_b) && loop_b == 50, "FAT12 cyclic chain left unchanged");

    make_volume(1, 6000, 1, 2, 24, 512, 1, FAT_NATIVE_FAT16);
    fat_native_volume fat16;
    passed &= require(fat_native_mount(&fat16, 0, 1) && fat16.fat_bits == FAT_NATIVE_FAT16, "FAT16 geometry mount");
    first = 0;
    passed &= require(fat_native_allocate_chain(&fat16, 2, &first), "FAT16 chain allocation");
    passed &= require(fat_native_chain_next(&fat16, first, &next) == FAT_NATIVE_CHAIN_NEXT, "FAT16 chain link");
    passed &= require(fat_native_chain_next(&fat16, next, &value) == FAT_NATIVE_CHAIN_END, "FAT16 end-of-chain marker");
    passed &= require(fat_native_release_chain(&fat16, first), "FAT16 chain release");

    make_volume(2, 70000, 32, 2, 600, 0, 1, FAT_NATIVE_FAT32);
    u64 fat32_start = 100 + 32;
    set_fat32_raw(fat32_start, 2, 0xAFFFFFFF);
    fat_native_volume fat32;
    passed &= require(fat_native_mount(&fat32, 0, 2) && fat32.fat_bits == FAT_NATIVE_FAT32 && fat32.root_cluster == 2, "FAT32 geometry mount");
    passed &= require(fat_native_fat_read(&fat32, 2, &value) && value == 0x0FFFFFFF, "FAT32 high reserved bits masked on read");
    set_fat32_raw(fat32_start, 3, 0xA0000000U);
    passed &= require(fat_native_fat_write(&fat32, 3, 0x0FFFFFFF), "FAT32 mirrored entry write");
    passed &= require((get32(&image[fat32_start][12]) & 0xF0000000U) == 0xA0000000U, "FAT32 reserved high bits preserved");
    passed &= require((get32(&image[fat32_start][12]) & 0x0FFFFFFFU) == 0x0FFFFFFFU, "FAT32 end marker write");
    passed &= require((get32(&image[fat32_start + 600][12]) & 0x0FFFFFFFU) == 0x0FFFFFFFU, "FAT32 mirrored FAT copy");
    put16(&image[100][40], 0x0081);
    fat_native_volume active_fat;
    passed &= require(fat_native_mount(&active_fat, 0, 2) && !active_fat.mirrored && active_fat.active_fat == 1, "FAT32 active FAT selection");
    passed &= require(fat_native_fat_write(&active_fat, 4, 0x0FFFFFFF), "FAT32 active FAT write");
    passed &= require(get32(&image[fat32_start][16]) == 0 && get32(&image[fat32_start + 600][16]) == 0x0FFFFFFF, "FAT32 disabled mirroring respected");
    u64 cluster_lba = 0;
    passed &= require(fat_native_cluster_sector(&fat32, 2, 0, &cluster_lba) && cluster_lba == 100 + fat32.data_start, "FAT32 cluster-to-sector mapping");
    passed &= require(!fat_native_cluster_sector(&fat32, fat32.cluster_count + 2, 0, &cluster_lba), "FAT32 invalid cluster rejected");

    make_volume(3, 400, 1, 2, 2, 64, 3, FAT_NATIVE_FAT12);
    fat_native_volume invalid;
    passed &= require(!fat_native_mount(&invalid, 0, 3), "invalid sectors-per-cluster rejected");
    disks[0].parts[3].loc = TEST_SECTORS - 100;
    disks[0].parts[3].size = 400;
    passed &= require(!fat_native_mount(&invalid, 0, 3), "partition beyond disk rejected");

    printf("%s\n", passed ? "ALL NATIVE FAT CORE TESTS PASSED" : "NATIVE FAT CORE TESTS FAILED");
    return passed ? 0 : 1;
}
