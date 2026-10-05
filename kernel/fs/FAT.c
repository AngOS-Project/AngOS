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

static u16 fat16_next_cluster(u32 fat_lba, u16 cluster, unsigned int disk) {
    u8 sector[512];

    u32 offset = (u32)cluster * 2;
    u32 lba = fat_lba + offset / 512;
    u32 pos = offset % 512;

    if (!disk_read_sector(lba, sector, disk))
        return 0xFFFF;

    return (u16)sector[pos] |
           ((u16)sector[pos + 1] << 8);
}

static bool fat16_find_in_dir(
    unsigned int disk,
    u32 root_lba,
    u32 root_sectors,
    u32 data_lba,
    u8 sectors_per_cluster,
    u32 fat_lba,
    bool root,
    u16 dir_cluster,
    const char *name,
    u8 *result
) {
    u8 sector[512];

    if (root) {
        for (u32 s = 0; s < root_sectors; ++s) {
            if (!disk_read_sector(root_lba + s, sector, disk))
                return false;

            for (u32 off = 0; off < 512; off += 32) {
                u8 first = sector[off];

                if (first == 0x00)
                    return false;

                if (first == 0xE5)
                    continue;

                u8 attr = sector[off + 11];

                if (attr == 0x0F || (attr & 0x08))
                    continue;

                if (fat_name_equal(
                        name,
                        &sector[off],
                        &sector[off + 8])) {

                    memcpy(result, &sector[off], 32);
                    return true;
                }
            }
        }

        return false;
    }

    u16 cluster = dir_cluster;

    while (cluster >= 2 && cluster < 0xFFF8) {
        u32 cluster_lba =
            data_lba +
            ((u32)cluster - 2) * sectors_per_cluster;

        for (u32 s = 0; s < sectors_per_cluster; ++s) {
            if (!disk_read_sector(cluster_lba + s, sector, disk))
                return false;

            for (u32 off = 0; off < 512; off += 32) {
                u8 first = sector[off];

                if (first == 0x00)
                    return false;

                if (first == 0xE5)
                    continue;

                u8 attr = sector[off + 11];

                if (attr == 0x0F || (attr & 0x08))
                    continue;

                if (fat_name_equal(
                        name,
                        &sector[off],
                        &sector[off + 8])) {

                    memcpy(result, &sector[off], 32);
                    return true;
                }
            }
        }

        u16 next = fat16_next_cluster(fat_lba, cluster, disk);

        if (next == 0xFFFF ||
            next == 0xFFF7 ||
            next < 2)
            return false;

        cluster = next;
    }

    return false;
}

int fat16_find_file_entry(const FILE *fp, FAT_entry *entry) {
    if (!fp || !entry || !fp->path)
        return 0;

    unsigned int disk = volumes[fp->volume].disk;

    if (fat_detect(disk) != 16)
        return 0;

    u8 mbr[512];
    u8 boot[512];

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

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
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

    u32 part_count = 0;

    while (fp->path[part_count] != NULL) {
        ++part_count;

        if (part_count >= 16)
            return 0;
    }

    if (part_count == 0)
        return 0;

    bool root = true;
    u16 dir_cluster = 0;
    u8 found[32];

    for (u32 i = 0; i < part_count; ++i) {
        if (!fat16_find_in_dir(
                disk,
                root_lba,
                root_sectors,
                data_lba,
                sectors_per_cluster,
                fat_lba,
                root,
                dir_cluster,
                fp->path[i],
                found))
            return 0;

        bool directory =
            (found[11] & 0x10) != 0;

        if (i + 1 < part_count) {
            if (!directory)
                return 0;

            dir_cluster =
                (u16)found[26] |
                ((u16)found[27] << 8);

            if (dir_cluster < 2)
                return 0;

            root = false;
        } else {
            if (directory)
                return 0;

            memcpy(entry, found, sizeof(FAT_entry));
        }
    }

    return 1;
}

int fat16_dir_open(DIR *dir, const char *path) {
    if (!dir || !path)
        return 0;

    unsigned int disk =
        volumes[dir->volume].disk;

    if (fat_detect(disk) != 16)
        return 0;

    u8 mbr[512];
    u8 boot[512];

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

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
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
        fat_lba +
        ((u32)fat_count * sectors_per_fat);

    u32 data_lba =
        root_lba + root_sectors;

    fat16_dir_state_t *state =
        (fat16_dir_state_t *)dir->fsdata;

    memset(state, 0, sizeof(fat16_dir_state_t));

    state->root_lba = root_lba;
    state->root_sectors = root_sectors;
    state->fat_lba = fat_lba;
    state->data_lba = data_lba;
    state->sectors_per_cluster = sectors_per_cluster;

    const char *p = path;

    if (p[0] != '|' ||
        p[1] < 'A' ||
        p[1] > 'Z' ||
        p[2] != '/')
        return 0;

    p += 3;

    char copy[256];
    u32 length = 0;

    while (p[length] &&
           length < sizeof(copy) - 1) {
        copy[length] = p[length];
        ++length;
    }

    copy[length] = '\0';

    if (copy[0] == '\0') {
        state->root = true;
        state->root_sector = 0;
        state->entry_index = 0;
        state->done = false;
        return 1;
    }

    char *parts[16];
    u32 part_count = 0;
    char *cursor = copy;

    while (*cursor &&
           part_count < 16) {

        while (*cursor == '/')
            ++cursor;

        if (!*cursor)
            break;

        parts[part_count++] = cursor;

        while (*cursor &&
               *cursor != '/')
            ++cursor;

        if (*cursor)
            *cursor++ = '\0';
    }

    if (part_count == 0)
        return 0;

    bool root = true;
    u16 dir_cluster = 0;
    u8 found[32];

    for (u32 i = 0; i < part_count; ++i) {
        if (!fat16_find_in_dir(
                disk,
                root_lba,
                root_sectors,
                data_lba,
                sectors_per_cluster,
                fat_lba,
                root,
                dir_cluster,
                parts[i],
                found))
            return 0;

        if (!(found[11] & 0x10))
            return 0;

        dir_cluster =
            (u16)found[26] |
            ((u16)found[27] << 8);

        if (dir_cluster < 2)
            return 0;

        root = false;
    }

    state->root = false;
    state->cluster = dir_cluster;
    state->cluster_sector = 0;
    state->entry_index = 0;
    state->done = false;

    return 1;
}

int fat16_dir_read(DIR *dir) {
    if (!dir)
        return 0;

    fat16_dir_state_t *state =
        (fat16_dir_state_t *)dir->fsdata;

    unsigned int disk =
        volumes[dir->volume].disk;

    u8 sector[512];

    while (!state->done) {

        if (state->root) {

            if (state->root_sector >=
                state->root_sectors) {
                state->done = true;
                return 0;
            }

            if (!disk_read_sector(
                    state->root_lba +
                    state->root_sector,
                    sector,
                    disk)) {

                state->done = true;
                return 0;
            }

        } else {

            if (state->cluster < 2 ||
                state->cluster >= 0xFFF8) {

                state->done = true;
                return 0;
            }

            u32 lba =
                state->data_lba +
                ((u32)state->cluster - 2) *
                state->sectors_per_cluster +
                state->cluster_sector;

            if (!disk_read_sector(
                    lba,
                    sector,
                    disk)) {

                state->done = true;
                return 0;
            }
        }

        while (state->entry_index < 16) {
            u32 off =
                (u32)state->entry_index * 32;

            ++state->entry_index;

            u8 first = sector[off];

            if (first == 0x00) {
                state->done = true;
                return 0;
            }

            if (first == 0xE5)
                continue;

            u8 attr = sector[off + 11];

            if (attr == 0x0F ||
                (attr & 0x08))
                continue;

            char name[256];
            u32 n = 0;

            for (u32 i = 0;
                 i < 8 &&
                 sector[off + i] != ' ';
                 ++i) {

                name[n++] =
                    fat_upper(
                        sector[off + i]);
            }

            bool has_extension = false;

            for (u32 i = 0; i < 3; ++i) {
                if (sector[off + 8 + i] != ' ') {
                    has_extension = true;
                    break;
                }
            }

            if (has_extension) {
                name[n++] = '.';

                for (u32 i = 0;
                     i < 3 &&
                     sector[off + 8 + i] != ' ';
                     ++i) {

                    name[n++] =
                        fat_upper(
                            sector[off + 8 + i]);
                }
            }

            name[n] = '\0';

            if (strcmp(name, ".") == 0 ||
                strcmp(name, "..") == 0)
                continue;

            memcpy(
                dir->entry.name,
                name,
                n + 1
            );

            dir->entry.type =
                (attr & 0x10)
                ? DIRENT_DIR
                : DIRENT_FILE;

            dir->entry.size =
                (u32)sector[off + 28] |
                ((u32)sector[off + 29] << 8) |
                ((u32)sector[off + 30] << 16) |
                ((u32)sector[off + 31] << 24);

            return 1;
        }

        state->entry_index = 0;

        if (state->root) {
            ++state->root_sector;
        } else {
            ++state->cluster_sector;

            if (state->cluster_sector >=
                state->sectors_per_cluster) {

                u16 next =
                    fat16_next_cluster(
                        state->fat_lba,
                        state->cluster,
                        disk
                    );

                if (next < 2 ||
                    next == 0xFFF7 ||
                    next >= 0xFFF8) {

                    state->done = true;
                    return 0;
                }

                state->cluster = next;
                state->cluster_sector = 0;
            }
        }
    }

    return 0;
}

size_t fat16_read(void *ptr, size_t bytes, FILE *fp) {
    if (!ptr || !fp || !fp->path)
        return 0;

    if (fp->pointer >= fp->size)
        return 0;

    size_t available = fp->size - fp->pointer;

    if (bytes > available)
        bytes = available;

    if (bytes == 0)
        return 0;

    unsigned int disk = volumes[fp->volume].disk;

    u8 mbr[512];
    u8 boot[512];

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

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
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

    FAT_entry entry;

    if (!fat16_find_file_entry(fp, &entry))
        return 0;

    u16 cluster =
        (u16)entry.cluster_low;

    if (cluster < 2)
        return 0;

    u32 bytes_per_cluster =
        (u32)bytes_per_sector * sectors_per_cluster;

    u32 skipped_clusters =
        fp->pointer / bytes_per_cluster;

    u32 offset =
        fp->pointer % bytes_per_cluster;

    for (u32 i = 0; i < skipped_clusters; ++i) {
        cluster =
            fat16_next_cluster(
                fat_lba,
                cluster,
                disk
            );

        if (cluster < 2 || cluster >= 0xFFF8)
            return 0;
    }

    u8 sector[512];
    size_t remaining = bytes;
    size_t copied = 0;

    while (remaining > 0) {
        u32 cluster_lba =
            data_lba +
            ((u32)cluster - 2) * sectors_per_cluster;

        u32 cluster_offset = offset;

        for (u32 s = 0;
             s < sectors_per_cluster && remaining > 0;
             ++s) {

            if (!disk_read_sector(
                    cluster_lba + s,
                    sector,
                    disk))
                return copied;

            u32 sector_offset =
                cluster_offset >= 512
                ? cluster_offset - 512 * s
                : 0;

            if (s == 0)
                sector_offset = cluster_offset;

            if (sector_offset >= 512)
                continue;

            size_t count =
                512 - sector_offset;

            if (count > remaining)
                count = remaining;

            memcpy(
                (u8 *)ptr + copied,
                sector + sector_offset,
                count
            );

            copied += count;
            remaining -= count;
            cluster_offset = 0;
        }

        if (remaining == 0)
            break;

        cluster =
            fat16_next_cluster(
                fat_lba,
                cluster,
                disk
            );

        if (cluster < 2 || cluster >= 0xFFF8)
            break;

        offset = 0;
    }

    fp->pointer += copied;

    return copied;
}

int fat_cat(unsigned int disk, const char *filename) {
    u8 mbr[512];
    u8 boot[512];
    u8 sector[512];

    if (!filename || !*filename)
        return 0;

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

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
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

    char path[256];
    u32 length = 0;

    while (filename[length] && length < sizeof(path) - 1) {
        path[length] = filename[length];
        ++length;
    }

    path[length] = '\0';

    char *parts[16];
    u32 part_count = 0;
    char *p = path;

    while (*p && part_count < 16) {
        while (*p == '/')
            ++p;

        if (!*p)
            break;

        parts[part_count++] = p;

        while (*p && *p != '/')
            ++p;

        if (*p)
            *p++ = '\0';
    }

    if (part_count == 0)
        return 0;

    bool root = true;
    u16 dir_cluster = 0;
    u8 entry[32];

    for (u32 i = 0; i < part_count; ++i) {
        if (!fat16_find_in_dir(
                disk,
                root_lba,
                root_sectors,
                data_lba,
                sectors_per_cluster,
                fat_lba,
                root,
                dir_cluster,
                parts[i],
                entry))
            return 0;

        bool directory = (entry[11] & 0x10) != 0;

        if (i + 1 < part_count) {
            if (!directory)
                return 0;

            dir_cluster =
                (u16)entry[26] |
                ((u16)entry[27] << 8);

            if (dir_cluster < 2)
                return 0;

            root = false;
        } else {
            if (directory)
                return 0;
        }
    }

    u16 first_cluster =
        (u16)entry[26] |
        ((u16)entry[27] << 8);

    u32 file_size =
        (u32)entry[28] |
        ((u32)entry[29] << 8) |
        ((u32)entry[30] << 16) |
        ((u32)entry[31] << 24);

    terminal_write("=== FAT16 File === \n");

    if (file_size == 0)
        return 1;

    u32 remaining = file_size;
    u16 cluster = first_cluster;

    while (remaining > 0 &&
           cluster >= 2 &&
           cluster < 0xFFF8) {

        u32 cluster_lba =
            data_lba +
            ((u32)cluster - 2) * sectors_per_cluster;

        for (u32 s = 0;
             s < sectors_per_cluster && remaining > 0;
             ++s) {

            if (!disk_read_sector(cluster_lba + s, sector, disk))
                return 0;

            u32 count =
                remaining < 512 ? remaining : 512;

            for (u32 i = 0; i < count; ++i) {
                char c = (char)sector[i];

                if (c >= 32 ||
                    c == '\n' ||
                    c == '\r' ||
                    c == '\t') {

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

        cluster =
            fat16_next_cluster(
                fat_lba,
                cluster,
                disk);

        if (cluster == 0xFFFF)
            return 0;
    }

    terminal_write("\n");

    return remaining == 0;
}
