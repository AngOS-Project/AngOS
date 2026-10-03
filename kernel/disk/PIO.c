#include <types.h>
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

diskdata *disktable;
volume_t *volumes;

void smalldelay(void) {
    for (u8 i = 0; i < 15; ++i)
        inb(PRIMARY + STS);
}

static bool ata_wait_not_busy(u16 port) {
    for (u32 i = 0; i < 1000000; ++i) {
        if (!(inb(port + STS) & BSY))
            return true;
    }

    return false;
}

static bool ata_wait_drq(u16 port) {
    for (u32 i = 0; i < 1000000; ++i) {
        u8 status = inb(port + STS);

        if (status & ERROR)
            return false;

        if ((status & BSY) == 0 && (status & DRQ))
            return true;
    }

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

    *blocks =
        (u64)id[100] |
        ((u64)id[101] << 16) |
        ((u64)id[102] << 32) |
        ((u64)id[103] << 48);

    return *blocks != 0;
}

bool disk_read_sector(u64 LBA, void *buffer, unsigned int disk) {
    if (!buffer)
        return false;

    u16 *buf = (u16 *)buffer;

    u16 port = PRIMARY;
    u8 outcmd = 0x40;

    if (disk >= 2)
        port = SECOND;

    if (disk % 2)
        outcmd = 0x50;

    if (!ata_wait_not_busy(port))
        return false;

    outb(port + DHR, outcmd);

    outb(port + SEC, 0);
    outb(port + LBALO, (u8)(LBA >> 24));
    outb(port + LBAMID, (u8)(LBA >> 32));
    outb(port + LBAHI, (u8)(LBA >> 40));

    outb(port + SEC, 1);
    outb(port + LBALO, (u8)LBA);
    outb(port + LBAMID, (u8)(LBA >> 8));
    outb(port + LBAHI, (u8)(LBA >> 16));

    outb(port + CMD, READ);

    if (!ata_wait_not_busy(port))
        return false;

    if (!ata_wait_drq(port))
        return false;

    for (u16 i = 0; i < 256; ++i)
        buf[i] = inw(port + DAT);

    return true;
}

void readsectorpio(u64 LBA, u16 count, void *buffer, unsigned int disk) {
    u16 *buf = (u16 *)buffer;

    u16 port = PRIMARY;
    u8 outcmd = 0x40;

    if (disk >= 2)
        port = SECOND;

    if (disk % 2)
        outcmd = 0x50;

    while (inb(port + STS) & BSY) {
    }

    outb(port + DHR, outcmd);
    outb(port + SEC, (u8)(count >> 8));
    outb(port + LBALO, (u8)(LBA >> 24));
    outb(port + LBAMID, (u8)(LBA >> 32));
    outb(port + LBAHI, (u8)(LBA >> 40));
    outb(port + SEC, (u8)count);
    outb(port + LBALO, (u8)LBA);
    outb(port + LBAMID, (u8)(LBA >> 8));
    outb(port + LBAHI, (u8)(LBA >> 16));
    outb(port + CMD, READ);

    for (u16 i = 0; i < count; ++i) {
        smalldelay();

        while (inb(port + STS) & BSY) {
        }

        while (!(inb(port + STS) & DRQ)) {
        }

        for (u16 j = 0; j < 256; ++j) {
            *buf = inw(port + DAT);
            ++buf;
        }
    }
}

void writesectorpio(u64 LBA, u16 count, void *buffer, unsigned int disk) {
    u16 *buf = (u16 *)buffer;

    u16 port = PRIMARY;
    u8 outcmd = 0x40;

    if (disk >= 2)
        port = SECOND;

    if (disk % 2)
        outcmd = 0x50;

    while (inb(port + STS) & BSY) {
    }

    outb(port + DHR, outcmd);
    outb(port + SEC, (u8)(count >> 8));
    outb(port + LBALO, (u8)(LBA >> 24));
    outb(port + LBAMID, (u8)(LBA >> 32));
    outb(port + LBAHI, (u8)(LBA >> 40));
    outb(port + SEC, (u8)count);
    outb(port + LBALO, (u8)LBA);
    outb(port + LBAMID, (u8)(LBA >> 8));
    outb(port + LBAHI, (u8)(LBA >> 16));
    outb(port + CMD, WRITE);

    for (u16 i = 0; i < count; ++i) {
        smalldelay();

        while (inb(port + STS) & BSY) {
        }

        while (!(inb(port + STS) & DRQ)) {
        }

        for (u16 j = 0; j < 256; ++j) {
            outw(port + DAT, *buf);
            ++buf;
        }
    }
}

void init_disk(void) {
    disktable = malloc(sizeof(diskdata) * DISKS);
    volumes = malloc(sizeof(volume_t) * LETTERS);

    if (!disktable || !volumes)
        return;

    memset(disktable, 0, sizeof(diskdata) * DISKS);
    memset(volumes, 0, sizeof(volume_t) * LETTERS);

    disktable[0].parts[0].loc = 0;

    volumes[0].disk = 0;
    volumes[0].partition = 0;

    int type = fat_detect(0);

    if (type == 12)
        disktable[0].parts[0].fs = fat12;
    else if (type == 16)
        disktable[0].parts[0].fs = fat16;
    else if (type == 32)
        disktable[0].parts[0].fs = fat32;
    else
        disktable[0].parts[0].fs = none;
}