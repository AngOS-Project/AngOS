#include <types.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>
#include <FS.h>
#include "FAT.h"
#include <terminal.h>

static void wcatomba(u8 *dest, const wchar *src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        u16 c = src[i];

        if (c <= 0x7F)
            dest[i] = (u8)c;
        else
            dest[i] = '?';
    }
}

void fat_setup(unsigned int disk, unsigned int part) {
	bootrecord BPB;
	fat_fs_info fs_info;
	partinfo *info = &disktable[disk].parts[part];
	u32 FAT_buffer[SECTOR_SIZE / 4];

	readsectorpio(info->loc, 1, &BPB, disk);

	fs_info.bytes_per_sector = BPB.bytes_per_sector;
	fs_info.sectors_per_cluster = BPB.sectors_per_cluster;
	fs_info.sectors_per_fat = BPB.sectors_per_FAT;

	fs_info.root_cluster = BPB.root_cluster;
	fs_info.root_entries = BPB.root_entries;
	fs_info.root_sectors = (fs_info.root_entries * 32 + (fs_info.bytes_per_sector - 1)) / fs_info.bytes_per_sector;

	fs_info.fat_count = BPB.FAT_count;

	fs_info.fat = (BPB.reserved_sectors * fs_info.bytes_per_sector) / SECTOR_SIZE;
	fs_info.data = fs_info.fat + (fs_info.fat_count * ((fs_info.sectors_per_fat * fs_info.bytes_per_sector) / SECTOR_SIZE)) - 2;

	/* Checking how many clusters the root dir takes up */

	fs_info.root_clusters = 1;
	u32 current_cluster = fs_info.root_cluster;
	readsectorpio(info->loc + fs_info.fat + current_cluster / FEPS, 1, FAT_buffer, disk);
	do {
		if (current_cluster / FEPS != FAT_buffer[current_cluster] / FEPS)
			readsectorpio(info->loc + fs_info.fat + current_cluster / FEPS, 1, FAT_buffer, disk);
		if ((FAT_buffer[current_cluster % FEPS] & 0x0FFFFFFF) < 0x0FFFFFF0) {
			++fs_info.root_clusters;
			current_cluster = FAT_buffer[current_cluster % FEPS];
		}
		else break;
	} while (1);

	if (BPB.sector_count_small)
		fs_info.sector_count = BPB.sector_count_small;
	else
		fs_info.sector_count = BPB.sector_count_large;

	info->fs = fat32;
	info->fsdata = malloc(sizeof(fat_fs_info));
	memcpy(info->fsdata, &fs_info, sizeof(fat_fs_info));
}



size_t fat_read(void *ptr, size_t bytes, FILE *fp) {
	unsigned int disk = volumes[fp->volume].disk;
	unsigned int part = volumes[fp->volume].partition;
	LBA partoffset = disktable[disk].parts[part].loc;

	const size_t return_value = bytes;

	fat_fs_info *fs_info = (fat_fs_info *) disktable[disk].parts[part].fsdata;
	const u32 bytes_per_cluster = fs_info->bytes_per_sector * fs_info->sectors_per_cluster;
	u32 skipped_clusters = fp->pointer / bytes_per_cluster;

	u32 *FAT_buffer = malloc(SECTOR_SIZE);
	u8 *buffer = malloc(bytes_per_cluster);
	char filename[256];

	FAT_entry entry;
	u32 active_cluster;

	find_fat_entry(&entry, filename, fp);
	active_cluster = entry.cluster_high << 16 | entry.cluster_low;

	readsectorpio(partoffset + fs_info->fat + active_cluster / FEPS, 1, FAT_buffer, disk);

	for (u32 i = 0; i < skipped_clusters; ++i) {
		if (active_cluster / FEPS != FAT_buffer[active_cluster % FEPS] / FEPS) {
			readsectorpio(partoffset + fs_info->fat + FAT_buffer[active_cluster % FEPS] / FEPS, fs_info->sectors_per_cluster, FAT_buffer, disk);
		}
		active_cluster = FAT_buffer[active_cluster % FEPS];
	}

	if (fp->pointer % bytes_per_cluster) {
		readsectorpio(partoffset + fs_info->data + active_cluster * fs_info->sectors_per_cluster, (fs_info->bytes_per_sector / SECTOR_SIZE) * fs_info->sectors_per_cluster, buffer, disk);

		if (bytes > bytes_per_cluster - fp->pointer % bytes_per_cluster) {
			memcpy(ptr, buffer + fp->pointer % bytes_per_cluster, bytes_per_cluster - fp->pointer % bytes_per_cluster);
			ptr += bytes_per_cluster - fp->pointer % bytes_per_cluster;
			bytes -= bytes_per_cluster - fp->pointer % bytes_per_cluster;
		}
		else {
			memcpy(ptr, buffer + fp->pointer % bytes_per_cluster, bytes);
			return return_value;
		}

		if (active_cluster / FEPS != FAT_buffer[active_cluster % FEPS] / FEPS) {
			readsectorpio(partoffset + fs_info->fat + FAT_buffer[active_cluster % FEPS] / FEPS, fs_info->sectors_per_cluster, FAT_buffer, disk);
		}
		active_cluster = FAT_buffer[active_cluster % FEPS];
	}

	do {
		readsectorpio(partoffset + fs_info->data + active_cluster * fs_info->sectors_per_cluster, (fs_info->bytes_per_sector / SECTOR_SIZE) * fs_info->sectors_per_cluster, buffer, disk);
		if (bytes > bytes_per_cluster) {
			memcpy(ptr, buffer, bytes_per_cluster);
			bytes -= bytes_per_cluster;
		}
		else {
			memcpy(ptr, buffer, bytes);
			break;
		}
		ptr += bytes_per_cluster;

		if (active_cluster / FEPS != FAT_buffer[active_cluster % FEPS] / FEPS) {
			readsectorpio(partoffset + fs_info->fat + FAT_buffer[active_cluster % FEPS] / FEPS, fs_info->sectors_per_cluster, FAT_buffer, disk);
		}
		else
			active_cluster = FAT_buffer[active_cluster % FEPS];

	} while ((active_cluster & 0x0FFFFFFF) < 0x0FFFFFF0);

	free(FAT_buffer);
	return return_value;
}



int find_fat_entry(FAT_entry *entry, char filename[256], const FILE *fp) {
	/* entry and name parameters are both outputs */

	char volume = fp->volume;

	unsigned int disk = volumes[volume].disk;
	unsigned int part = volumes[volume].partition;
	LBA partoffset = disktable[disk].parts[part].loc;
	fat_fs_info *fs_info = (fat_fs_info *) disktable[disk].parts[part].fsdata;

	unsigned int i, j, len = 0;

	char charbuf[6];
	unsigned int depth = 0;

	u32 active_cluster = fs_info->root_cluster;
	u32 *FAT_buffer = malloc(SECTOR_SIZE);
	const u32 bytes_per_cluster = fs_info->bytes_per_sector * fs_info->sectors_per_cluster;
	FAT_entry *buffer = malloc(bytes_per_cluster);

	FAT_entry *current;

	readsectorpio(partoffset + fs_info->fat + active_cluster / FEPS, 1, FAT_buffer, disk);

	do {
inloop:
		readsectorpio(partoffset + fs_info->data + active_cluster * fs_info->sectors_per_cluster, fs_info->sectors_per_cluster, buffer, disk);
inloop2:
		current = buffer;

		/* Under this, everything to right of '<' is the amount of fat entries in a cluster */
		for (i = 0; i < bytes_per_cluster / sizeof(FAT_entry); ++i) {
			if (current->attr == LFN) {
				while (current->attr == LFN) {
					wcatomba((u8*) charbuf, ((LFN_entry*) current)->first, 5);
					memcpy(&filename[(((((LFN_entry*) current)->order) & 0x1F) - 1) * 13], charbuf, 5);

					wcatomba((u8*) charbuf, ((LFN_entry*) current)->middle, 6);
					memcpy(&filename[(((((LFN_entry*) current)->order) & 0x1F) - 1) * 13 + 5], charbuf, 6);

					wcatomba((u8*) charbuf, ((LFN_entry*) current)->last, 2);
					memcpy(&filename[(((((LFN_entry*) current)->order) & 0x1F) - 1) * 13 + 11], charbuf, 2);

					len += 13;
					++current;
					++i;

					if (i >= bytes_per_cluster / sizeof(FAT_entry)) {
						readsectorpio(partoffset + fs_info->fat + FAT_buffer[active_cluster % FEPS] / FEPS, fs_info->sectors_per_cluster, FAT_buffer, disk);
						active_cluster = FAT_buffer[active_cluster % FEPS];
						goto inloop2;
					}
				}
				filename[len] = '\0';
				len = 0;
			}
			else {
				for (j = 0; (filename[j] = current->name[j]) != ' ' && j < 8; ++j);
				filename[j] = '.';
				memcpy(&filename[j + 1], current->extension, 3);
				filename[j + 4] = '\0';
			}

			if (!strcmp(filename, fp->path[depth])) {
				active_cluster =  current->cluster_high << 16 | current->cluster_low;
				memcpy(entry, current, sizeof(FAT_entry));
				if (!(current->attr & 0x10)) return 0;
				++depth;
				goto inloop;
			}
			++current;
		}
		readsectorpio(partoffset + fs_info->fat + FAT_buffer[active_cluster % FEPS] / FEPS, fs_info->sectors_per_cluster, FAT_buffer, disk);
		active_cluster = FAT_buffer[active_cluster % FEPS];
	} while ((active_cluster & 0x0FFFFFFF) < 0x0FFFFFF0);

	return 2;
}

unsigned long long fat_filesize(FILE *fp) {
	FAT_entry entry;
	char filename[256];
	find_fat_entry(&entry, filename, fp);
	return (fp->size = entry.size);
}

int fat_detect(unsigned int disk) {
    u8 mbr[512];
    u8 boot[512];

    if (!disk_read_sector(0, mbr, disk))
        return 0;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return 0;

    u8 partition_type = mbr[446 + 4];

    u32 partition_lba =
        (u32)mbr[446 + 8] |
        ((u32)mbr[446 + 9] << 8) |
        ((u32)mbr[446 + 10] << 16) |
        ((u32)mbr[446 + 11] << 24);

    if (partition_lba == 0)
        return 0;

    if (!disk_read_sector(partition_lba, boot, disk))
        return 0;

    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return 0;

    u16 bytes_per_sector =
        (u16)boot[11] |
        ((u16)boot[12] << 8);

    u8 sectors_per_cluster = boot[13];
    u16 reserved_sectors =
        (u16)boot[14] |
        ((u16)boot[15] << 8);

    u8 fat_count = boot[16];

    if (bytes_per_sector != 512)
        return 0;

    if (sectors_per_cluster == 0)
        return 0;

    if (reserved_sectors == 0)
        return 0;

    if (fat_count == 0)
        return 0;

    if (partition_type == 0x01)
        return 12;

    if (partition_type == 0x06 || partition_type == 0x0E) {
        u16 sectors_per_fat =
            (u16)boot[22] |
            ((u16)boot[23] << 8);

        if (sectors_per_fat == 0)
            return 0;

        return 16;
    }

    if (partition_type == 0x0B || partition_type == 0x0C) {
        u32 sectors_per_fat =
            (u32)boot[36] |
            ((u32)boot[37] << 8) |
            ((u32)boot[38] << 16) |
            ((u32)boot[39] << 24);

        u32 root_cluster =
            (u32)boot[44] |
            ((u32)boot[45] << 8) |
            ((u32)boot[46] << 16) |
            ((u32)boot[47] << 24);

        if (sectors_per_fat == 0)
            return 0;

        if (root_cluster < 2)
            return 0;

        return 32;
    }

    return 0;
}

bool fat_probe(unsigned int disk) {
    return fat_detect(disk) != 0;
}

int fat_list_root(unsigned int disk) {
    u8 mbr[512];
    u8 boot[512];
    u8 sector[512];

    if (fat_detect(disk) != 16)
        return 0;

    if (!disk_read_sector(0, mbr, disk))
        return 0;

    u32 partition_lba =
        (u32)mbr[446 + 8] |
        ((u32)mbr[446 + 9] << 8) |
        ((u32)mbr[446 + 10] << 16) |
        ((u32)mbr[446 + 11] << 24);

    if (!disk_read_sector(partition_lba, boot, disk))
        return 0;

    u16 bytes_per_sector =
        (u16)boot[11] |
        ((u16)boot[12] << 8);

    u16 reserved_sectors =
        (u16)boot[14] |
        ((u16)boot[15] << 8);

    u8 fat_count = boot[16];

    u16 root_entries =
        (u16)boot[17] |
        ((u16)boot[18] << 8);

    u16 sectors_per_fat =
        (u16)boot[22] |
        ((u16)boot[23] << 8);

    if (bytes_per_sector != 512 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0)
        return 0;

    u32 root_sectors =
        ((u32)root_entries * 32 + bytes_per_sector - 1) /
        bytes_per_sector;

    u32 root_lba =
        partition_lba +
        reserved_sectors +
        ((u32)fat_count * sectors_per_fat);

    terminal_write("=== FAT16 Root Directory === \n");

    for (u32 s = 0; s < root_sectors; ++s) {
        if (!disk_read_sector(root_lba + s, sector, disk))
            return 0;

        for (u32 off = 0; off < 512; off += 32) {
            u8 first = sector[off];

            if (first == 0x00)
                return 1;

            if (first == 0xE5)
                continue;

            u8 attr = sector[off + 11];

            if (attr == 0x0F)
                continue;

            if (attr & 0x08)
                continue;

            char name[13];
            u32 n = 0;

            for (u32 i = 0; i < 8 && sector[off + i] != ' '; ++i)
                name[n++] = sector[off + i];

            bool has_extension = false;

            for (u32 i = 0; i < 3; ++i) {
                if (sector[off + 8 + i] != ' ') {
                    has_extension = true;
                    break;
                }
            }

            if (has_extension) {
                name[n++] = '.';

                for (u32 i = 0; i < 3 && sector[off + 8 + i] != ' '; ++i)
                    name[n++] = sector[off + 8 + i];
            }

            name[n] = '\0';

            terminal_write("  ");
            terminal_write(name);

            if (attr & 0x10)
                terminal_write("/");

            terminal_write("\n");
        }
    }

    return 1;
}

static char fat_upper(char c) {
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');

    return c;
}

static bool fat_name_equal(const char *input, const u8 *name, const u8 *ext) {
    char target[13];
    u32 n = 0;

    for (u32 i = 0; i < 8 && name[i] != ' '; ++i)
        target[n++] = fat_upper(name[i]);

    bool has_ext = false;

    for (u32 i = 0; i < 3; ++i) {
        if (ext[i] != ' ') {
            has_ext = true;
            break;
        }
    }

    if (has_ext) {
        target[n++] = '.';

        for (u32 i = 0; i < 3 && ext[i] != ' '; ++i)
            target[n++] = fat_upper(ext[i]);
    }

    target[n] = '\0';

    u32 i = 0;

    while (input[i] && target[i]) {
        if (fat_upper(input[i]) != target[i])
            return false;

        ++i;
    }

    return input[i] == '\0' && target[i] == '\0';
}

int fat_cat(unsigned int disk, const char *filename) {
    u8 mbr[512];
    u8 boot[512];
    u8 sector[512];

    if (fat_detect(disk) != 16)
        return 0;

    if (!disk_read_sector(0, mbr, disk))
        return 0;

    u32 partition_lba =
        (u32)mbr[446 + 8] |
        ((u32)mbr[446 + 9] << 8) |
        ((u32)mbr[446 + 10] << 16) |
        ((u32)mbr[446 + 11] << 24);

    if (!disk_read_sector(partition_lba, boot, disk))
        return 0;

    u16 bytes_per_sector =
        (u16)boot[11] |
        ((u16)boot[12] << 8);

    u8 sectors_per_cluster = boot[13];

    u16 reserved_sectors =
        (u16)boot[14] |
        ((u16)boot[15] << 8);

    u8 fat_count = boot[16];

    u16 root_entries =
        (u16)boot[17] |
        ((u16)boot[18] << 8);

    u16 sectors_per_fat =
        (u16)boot[22] |
        ((u16)boot[23] << 8);

    if (bytes_per_sector != 512)
        return 0;

    if (sectors_per_cluster == 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0)
        return 0;

    u32 root_sectors =
        ((u32)root_entries * 32 + 511) / 512;

    u32 fat_lba =
        partition_lba + reserved_sectors;

    u32 root_lba =
        fat_lba + ((u32)fat_count * sectors_per_fat);

    u32 data_lba =
        root_lba + root_sectors;

    u16 first_cluster = 0;
    u32 file_size = 0;
    bool found = false;

    for (u32 s = 0; s < root_sectors && !found; ++s) {
        if (!disk_read_sector(root_lba + s, sector, disk))
            return 0;

        for (u32 off = 0; off < 512; off += 32) {
            u8 first = sector[off];

            if (first == 0x00)
                break;

            if (first == 0xE5)
                continue;

            u8 attr = sector[off + 11];

            if (attr == 0x0F || (attr & 0x08))
                continue;

            if (attr & 0x10)
                continue;

            if (fat_name_equal(
                    filename,
                    &sector[off],
                    &sector[off + 8])) {

                first_cluster =
                    (u16)sector[off + 26] |
                    ((u16)sector[off + 27] << 8);

                file_size =
                    (u32)sector[off + 28] |
                    ((u32)sector[off + 29] << 8) |
                    ((u32)sector[off + 30] << 16) |
                    ((u32)sector[off + 31] << 24);

                found = true;
                break;
            }
        }
    }

    if (!found)
        return 0;

    terminal_write("=== FAT16 File === \n");

    if (file_size == 0)
        return 1;

    u32 remaining = file_size;
    u16 cluster = first_cluster;

    while (remaining > 0 && cluster >= 2 && cluster < 0xFFF8) {
        u32 cluster_lba =
            data_lba +
            ((u32)cluster - 2) * sectors_per_cluster;

        for (u32 s = 0;
             s < sectors_per_cluster && remaining > 0;
             ++s) {

            if (!disk_read_sector(cluster_lba + s, sector, disk))
                return 0;

            u32 count = remaining < 512 ? remaining : 512;

            for (u32 i = 0; i < count; ++i) {
                char c = (char)sector[i];

                if (c >= 32 || c == '\n' || c == '\r' || c == '\t') {
                    char out[2];
                    out[0] = c;
                    out[1] = '\0';
                    terminal_write(out);
                }
            }

            remaining -= count;
        }

        if (remaining == 0)
            break;

        u32 fat_offset = (u32)cluster * 2;
        u32 fat_sector = fat_lba + fat_offset / 512;
        u32 fat_offset_in_sector = fat_offset % 512;

        if (!disk_read_sector(fat_sector, sector, disk))
            return 0;

        cluster =
            (u16)sector[fat_offset_in_sector] |
            ((u16)sector[fat_offset_in_sector + 1] << 8);
    }

    terminal_write("\n");

    return 1;
}
