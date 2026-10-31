#include <types.h>

#include <diagnostics.h>
#include <string.h>
#include <stdlib.h>
#include <cpu/IO.h>
#include <syscall.h>
#include <FS.h>
#include "../fs/FAT.h"

#define PRIMARY 0x1F0
#define SECOND  0x170

#define DAT     0
#define ERR     1
#define FEA     1
#define SEC     2
#define LBALO   3
#define LBAMID  4
#define LBAHI   5
#define DHR     6
#define STS     7
#define CMD     7

#define ERROR    0x01
#define IDX      0x02
#define CORR     0x04
#define DRQ      0x08
#define SRV      0x10
#define DF       0x20
#define RDY      0x40
#define BSY      0x80

#define READ      0x24
#define WRITE     0x34
#define IDENTIFY  0xEC

#define DISKS 4

static u64 disk_capacity[DISKS];

diskdata *disktable;
volume_t *volumes;

void smalldelay(void) {
    for (u8 i = 0; i < 15; ++i)
        inb(PRIMARY + STS);
}

static bool ata_wait_not_busy(u16 port) {
    for (u32 i = 0; i < 1000000; ++i) {
        u8 status = inb(port + STS);
        if (status == 0)
            return false;
        if (!(status & BSY))
            return !(status & (ERROR | DF));
    }
    return false;
}

static bool ata_wait_drq(u16 port) {
    for (u32 i = 0; i < 1000000; ++i) {
        u8 status = inb(port + STS);
        if (status == 0 || (status & (ERROR | DF)))
            return false;
        if (!(status & BSY) && (status & DRQ))
            return true;
    }
    return false;
}

static bool ata_request_valid(u64 LBA, u32 count, unsigned int disk) {
    const u64 max_lba = 0x0000FFFFFFFFFFFFULL;
    if (disk >= DISKS || count == 0 || count > 128 || LBA > max_lba || (u64)(count - 1) > max_lba - LBA) {
        diagnostics_log("Disk: invalid drive, sector range or transfer count.");
        return false;
    }
    u64 capacity = disk_capacity[disk];
    if (!capacity && disktable)
        capacity = disktable[disk].blocks;
    if (capacity && (LBA >= capacity || (u64)count > capacity - LBA)) {
        diagnostics_log("Disk: request exceeds the reported drive capacity.");
        return false;
    }
    return true;
}

static bool disk_read_failed(void *buffer, const char *message) {
    memset(buffer, 0, SECTOR_SIZE);
    diagnostics_log(message);
    return false;
}

bool disk_probe(u64 *blocks) {
    if (!blocks)
        return false;

    *blocks = 0;

    u16 port = PRIMARY;
    u16 id[256];

    outb(port + DHR, 0xA0);

    for (u8 i = 0; i < 5; ++i)
        inb(port + STS);

    outb(port + SEC, 0);
    outb(port + LBALO, 0);
    outb(port + LBAMID, 0);
    outb(port + LBAHI, 0);
    outb(port + CMD, IDENTIFY);

    u8 status = inb(port + STS);

    if (status == 0)
        return false;

    if (!ata_wait_not_busy(port))
        return false;

    if (inb(port + LBAMID) != 0 || inb(port + LBAHI) != 0)
        return false;

    if (!ata_wait_drq(port))
        return false;

    for (u16 i = 0; i < 256; ++i)
        id[i] = inw(port + DAT);

    u8 final_status = inb(port + STS);
    if (final_status == 0 || (final_status & (BSY | ERROR | DF)))
        return false;

    *blocks =
        (u64)id[100] |
        ((u64)id[101] << 16) |
        ((u64)id[102] << 32) |
        ((u64)id[103] << 48);

    if (*blocks != 0)
        disk_capacity[0] = *blocks;
    return *blocks != 0;
}

bool disk_read_sector(u64 LBA, void *buffer, unsigned int disk) {
    if (!buffer)
        return false;
    if (!ata_request_valid(LBA, 1, disk))
        return disk_read_failed(buffer, "Disk: rejected a sector read request.");

    u16 *buf = (u16 *)buffer;
    u16 port = disk >= 2 ? SECOND : PRIMARY;
    u8 select = disk % 2 ? 0x50 : 0x40;

    if (!ata_wait_not_busy(port))
        return disk_read_failed(buffer, "Disk: drive unavailable before sector read.");

    outb(port + DHR, select);
    for (u8 i = 0; i < 5; ++i)
        inb(port + STS);
    outb(port + SEC, 0);
    outb(port + LBALO, (u8)(LBA >> 24));
    outb(port + LBAMID, (u8)(LBA >> 32));
    outb(port + LBAHI, (u8)(LBA >> 40));
    outb(port + SEC, 1);
    outb(port + LBALO, (u8)LBA);
    outb(port + LBAMID, (u8)(LBA >> 8));
    outb(port + LBAHI, (u8)(LBA >> 16));
    outb(port + CMD, READ);

    if (!ata_wait_not_busy(port) || !ata_wait_drq(port))
        return disk_read_failed(buffer, "Disk: sector read timed out or reported an ATA error.");

    for (u16 i = 0; i < 256; ++i)
        buf[i] = inw(port + DAT);

    if (!ata_wait_not_busy(port))
        return disk_read_failed(buffer, "Disk: sector read did not finish before timeout.");
    u8 final_status = inb(port + STS);
    if (final_status == 0 || (final_status & (BSY | ERROR | DF)))
        return disk_read_failed(buffer, "Disk: sector read failed its final status check.");

    return true;
}

bool disk_write_sector(u64 LBA, const void *buffer, unsigned int disk) {
    if (!buffer || !ata_request_valid(LBA, 1, disk)) {
        diagnostics_log("Disk: rejected a sector write request.");
        return false;
    }

    const u16 *buf = (const u16 *)buffer;
    u16 port = disk >= 2 ? SECOND : PRIMARY;
    u8 select = disk % 2 ? 0x50 : 0x40;

    if (!ata_wait_not_busy(port)) {
        diagnostics_log("Disk: drive unavailable before sector write.");
        return false;
    }

    outb(port + DHR, select);
    for (u8 i = 0; i < 5; ++i)
        inb(port + STS);
    outb(port + SEC, 0);
    outb(port + LBALO, (u8)(LBA >> 24));
    outb(port + LBAMID, (u8)(LBA >> 32));
    outb(port + LBAHI, (u8)(LBA >> 40));
    outb(port + SEC, 1);
    outb(port + LBALO, (u8)LBA);
    outb(port + LBAMID, (u8)(LBA >> 8));
    outb(port + LBAHI, (u8)(LBA >> 16));
    outb(port + CMD, WRITE);

    if (!ata_wait_not_busy(port) || !ata_wait_drq(port)) {
        diagnostics_log("Disk: sector write timed out or reported an ATA error before data transfer.");
        return false;
    }

    for (u16 i = 0; i < 256; ++i)
        outw(port + DAT, buf[i]);

    if (!ata_wait_not_busy(port)) {
        diagnostics_log("Disk: sector write did not finish before timeout.");
        return false;
    }
    u8 final_status = inb(port + STS);
    if (final_status == 0 || (final_status & (BSY | ERROR | DF))) {
        diagnostics_log("Disk: sector write failed its final status check.");
        return false;
    }

    return true;
}

bool readsectorpio(u64 LBA, u16 count, void *buffer, unsigned int disk) {
    if (!buffer || !ata_request_valid(LBA, count, disk))
        return false;

    u16 *buf = (u16 *)buffer;
    u16 port = disk >= 2 ? SECOND : PRIMARY;
    u8 outcmd = disk % 2 ? 0x50 : 0x40;

    if (!ata_wait_not_busy(port)) {
        memset(buffer, 0, (size_t)count * SECTOR_SIZE);
        diagnostics_log("Disk: multi-sector read timed out before command issue.");
        return false;
    }

    outb(port + DHR, outcmd);
    for (u8 i = 0; i < 5; ++i)
        inb(port + STS);

    outb(port + SEC, (u8)(count >> 8));
    outb(port + LBALO, (u8)(LBA >> 24));
    outb(port + LBAMID, (u8)(LBA >> 32));
    outb(port + LBAHI, (u8)(LBA >> 40));
    outb(port + SEC, (u8)count);
    outb(port + LBALO, (u8)LBA);
    outb(port + LBAMID, (u8)(LBA >> 8));
    outb(port + LBAHI, (u8)(LBA >> 16));
    outb(port + CMD, READ);

    for (u32 sector = 0; sector < count; ++sector) {
        smalldelay();
        if (!ata_wait_drq(port)) {
            memset(buffer, 0, (size_t)count * SECTOR_SIZE);
            diagnostics_log("Disk: multi-sector read timed out or reported an ATA error.");
            return false;
        }
        for (u32 word = 0; word < 256; ++word)
            buf[sector * 256 + word] = inw(port + DAT);
    }

    if (!ata_wait_not_busy(port)) {
        memset(buffer, 0, (size_t)count * SECTOR_SIZE);
        diagnostics_log("Disk: multi-sector read did not finish before timeout.");
        return false;
    }

    u8 status = inb(port + STS);
    if (status == 0 || (status & (BSY | ERROR | DF))) {
        memset(buffer, 0, (size_t)count * SECTOR_SIZE);
        diagnostics_log("Disk: multi-sector read ended with an ATA device error.");
        return false;
    }

    return true;
}

bool writesectorpio(u64 LBA, u16 count, void *buffer, unsigned int disk) {
    if (!buffer || !ata_request_valid(LBA, count, disk))
        return false;

    const u16 *buf = (const u16 *)buffer;
    u16 port = disk >= 2 ? SECOND : PRIMARY;
    u8 outcmd = disk % 2 ? 0x50 : 0x40;

    if (!ata_wait_not_busy(port)) {
        diagnostics_log("Disk: multi-sector write timed out before command issue.");
        return false;
    }

    outb(port + DHR, outcmd);
    for (u8 i = 0; i < 5; ++i)
        inb(port + STS);

    outb(port + SEC, (u8)(count >> 8));
    outb(port + LBALO, (u8)(LBA >> 24));
    outb(port + LBAMID, (u8)(LBA >> 32));
    outb(port + LBAHI, (u8)(LBA >> 40));
    outb(port + SEC, (u8)count);
    outb(port + LBALO, (u8)LBA);
    outb(port + LBAMID, (u8)(LBA >> 8));
    outb(port + LBAHI, (u8)(LBA >> 16));
    outb(port + CMD, WRITE);

    for (u32 sector = 0; sector < count; ++sector) {
        smalldelay();
        if (!ata_wait_drq(port)) {
            diagnostics_log("Disk: multi-sector write timed out or reported an ATA error.");
            return false;
        }
        for (u32 word = 0; word < 256; ++word)
            outw(port + DAT, buf[sector * 256 + word]);
    }

    if (!ata_wait_not_busy(port)) {
        diagnostics_log("Disk: multi-sector write did not finish before timeout.");
        return false;
    }

    u8 status = inb(port + STS);
    if (status == 0 || (status & (BSY | ERROR | DF))) {
        diagnostics_log("Disk: multi-sector write ended with an ATA device error.");
        return false;
    }

    return true;
}

void init_disk(void) {
    disktable = malloc(sizeof(diskdata) * DISKS);
    volumes = malloc(sizeof(volume_t) * LETTERS);

    if (!disktable || !volumes) {
        if (disktable) {
            free(disktable);
            disktable = 0;
        }
        if (volumes) {
            free(volumes);
            volumes = 0;
        }
        diagnostics_log("Disk: allocation of disk or volume tables failed.");
        return;
    }

    memset(disktable, 0, sizeof(diskdata) * DISKS);
    memset(volumes, 0, sizeof(volume_t) * LETTERS);

    volumes[0].disk = 0;
    volumes[0].partition = 0;

    u64 detected_blocks = 0;
    if (disk_probe(&detected_blocks)) {
        disktable[0].blocks = detected_blocks;
        disk_capacity[0] = detected_blocks;
    }

    u8 secondary_mbr[SECTOR_SIZE];
    if (disk_read_sector(0, secondary_mbr, 1) &&
        secondary_mbr[510] == 0x55 &&
        secondary_mbr[511] == 0xAA) {
        u32 secondary_lba =
            (u32)secondary_mbr[454] |
            ((u32)secondary_mbr[455] << 8) |
            ((u32)secondary_mbr[456] << 16) |
            ((u32)secondary_mbr[457] << 24);
        u32 secondary_size =
            (u32)secondary_mbr[458] |
            ((u32)secondary_mbr[459] << 8) |
            ((u32)secondary_mbr[460] << 16) |
            ((u32)secondary_mbr[461] << 24);
        u64 secondary_end = (u64)secondary_lba + secondary_size;

        if (secondary_lba != 0 && secondary_size != 0 &&
            secondary_end <= 0x100000000ULL) {
            partinfo *secondary = &disktable[1].parts[0];
            secondary->loc = secondary_lba;
            secondary->size = secondary_size;

            int secondary_type = fat_detect(1);
            if (secondary_type == 12)
                secondary->fs = fat12;
            else if (secondary_type == 16)
                secondary->fs = fat16;
            else if (secondary_type == 32 && fat_setup(1, 0))
                secondary->fs = fat32;

            if (secondary->fs != none) {
                volumes[1].disk = 1;
                volumes[1].partition = 0;
                diagnostics_log("Disk: secondary filesystem detected and mounted.");
            }
        }
    }

    u8 mbr[SECTOR_SIZE];
    if (!disk_read_sector(0, mbr, 0) || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        diagnostics_log("Disk: MBR read failed or signature is invalid.");
        disktable[0].parts[0].fs = none;
        return;
    }

    u32 partition_lba =
        (u32)mbr[454] |
        ((u32)mbr[455] << 8) |
        ((u32)mbr[456] << 16) |
        ((u32)mbr[457] << 24);
    u32 partition_sectors =
        (u32)mbr[458] |
        ((u32)mbr[459] << 8) |
        ((u32)mbr[460] << 16) |
        ((u32)mbr[461] << 24);
    u64 partition_end = (u64)partition_lba + partition_sectors;

    if (partition_lba == 0 || partition_sectors == 0 || partition_end > 0x100000000ULL || (disktable[0].blocks != 0 && partition_end > disktable[0].blocks)) {
        diagnostics_log("Disk: first partition has invalid sector bounds.");
        disktable[0].parts[0].fs = none;
        return;
    }

    partinfo *info = &disktable[0].parts[0];
    info->loc = partition_lba;
    info->size = partition_sectors;

    int type = fat_detect(0);

    if (type == 12) {
        info->fs = fat12;
        return;
    }
    if (type == 16) {
        info->fs = fat16;
        return;
    }
    if (type == 32) {
        if (!fat_setup(0, 0)) {
            info->fs = none;
            diagnostics_log("Disk: FAT32 mount initialization failed.");
            return;
        }
        info->fs = fat32;
        return;
    }

    info->fs = none;
    diagnostics_log("Disk: no supported FAT filesystem found on the first partition.");
}
