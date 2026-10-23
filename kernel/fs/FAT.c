#include <types.h>
#include <diagnostics.h>
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

bool fat_setup(unsigned int disk, unsigned int part) {
    if (!disktable || disk >= 4 || part >= 4) {
        diagnostics_log("FAT32 setup: invalid disk or partition index.");
        return false;
    }
    partinfo *info = &disktable[disk].parts[part];
    bootrecord BPB;
    u32 FAT_buffer[SECTOR_SIZE / sizeof(u32)];
    if (!readsectorpio(info->loc, 1, &BPB, disk)) {
        diagnostics_log("FAT32 setup: boot-sector read failed.");
        return false;
    }
    if (BPB.bytes_per_sector != SECTOR_SIZE || BPB.sectors_per_cluster == 0 || BPB.sectors_per_cluster > 128 || (BPB.sectors_per_cluster & (BPB.sectors_per_cluster - 1)) != 0 || BPB.reserved_sectors == 0 || BPB.FAT_count == 0 || BPB.sectors_per_FAT == 0 || BPB.root_cluster < 2) {
        diagnostics_log("FAT32 setup: invalid boot-sector metadata.");
        return false;
    }
    fat_fs_info fs_info;
    memset(&fs_info, 0, sizeof(fs_info));
    fs_info.bytes_per_sector = BPB.bytes_per_sector;
    fs_info.sectors_per_cluster = BPB.sectors_per_cluster;
    fs_info.sectors_per_fat = BPB.sectors_per_FAT;
    fs_info.root_cluster = BPB.root_cluster;
    fs_info.root_entries = BPB.root_entries;
    fs_info.root_sectors = (fs_info.root_entries * 32 + fs_info.bytes_per_sector - 1) / fs_info.bytes_per_sector;
    fs_info.fat_count = BPB.FAT_count;
    fs_info.sector_count = BPB.sector_count_small ? BPB.sector_count_small : BPB.sector_count_large;
    if (fs_info.sector_count == 0) {
        diagnostics_log("FAT32 setup: volume reports no sectors.");
        return false;
    }
    u64 fat_start = BPB.reserved_sectors;
    u64 fat_length = (u64)fs_info.fat_count * fs_info.sectors_per_fat;
    u64 data_start = fat_start + fat_length;
    if (data_start < (u64)2 * fs_info.sectors_per_cluster || data_start >= fs_info.sector_count || data_start > 0xFFFFFFFFULL) {
        diagnostics_log("FAT32 setup: FAT or data region exceeds the volume bounds.");
        return false;
    }
    fs_info.fat = (u32)fat_start;
    fs_info.data = (u32)(data_start - (u64)2 * fs_info.sectors_per_cluster);
    u32 max_clusters = (fs_info.sector_count - (u32)data_start) / fs_info.sectors_per_cluster;
    if (max_clusters == 0 || (u64)max_clusters + 2 > (u64)fs_info.sectors_per_fat * FEPS) {
        diagnostics_log("FAT32 setup: cluster count exceeds the FAT or data region capacity.");
        return false;
    }
    if (fs_info.root_cluster < 2 || fs_info.root_cluster - 2 >= max_clusters) {
        diagnostics_log("FAT32 setup: root cluster is outside the volume.");
        return false;
    }
    fs_info.root_clusters = 0;
    u32 current_cluster = fs_info.root_cluster;
    bool reached_end = false;
    for (u32 step = 0; step < max_clusters; ++step) {
        if (current_cluster < 2 || current_cluster >= max_clusters + 2) {
            diagnostics_log("FAT32 setup: cluster chain contains an invalid cluster.");
            return false;
        }
        if (!readsectorpio(info->loc + fs_info.fat + current_cluster / FEPS, 1, FAT_buffer, disk)) {
            diagnostics_log("FAT32 setup: FAT-sector read failed.");
            return false;
        }
        u32 next = FAT_buffer[current_cluster % FEPS] & 0x0FFFFFFF;
        if (fs_info.root_clusters == 0xFFFF) {
            diagnostics_log("FAT32 setup: root directory cluster count exceeds the metadata limit.");
            return false;
        }
        ++fs_info.root_clusters;
        if (next >= 0x0FFFFFF8) {
            reached_end = true;
            break;
        }
        if (next < 2 || next >= 0x0FFFFFF0 || next >= max_clusters + 2) {
            diagnostics_log("FAT32 setup: root cluster chain is corrupt.");
            return false;
        }
        current_cluster = next;
    }
    if (!reached_end) {
        diagnostics_log("FAT32 setup: root cluster chain exceeded the volume limit.");
        return false;
    }
    fat_fs_info *stored = malloc(sizeof(fat_fs_info));
    if (!stored) {
        diagnostics_log("FAT32 setup: filesystem metadata allocation failed.");
        return false;
    }
    memcpy(stored, &fs_info, sizeof(fat_fs_info));
    info->fsdata = stored;
    info->fs = fat32;
    return true;
}

static bool fat32_next_cluster(LBA partoffset, const fat_fs_info *fs_info, u32 cluster, unsigned int disk, u32 *next_cluster) {
    u32 fat_buffer[SECTOR_SIZE / sizeof(u32)];
    if (!fs_info || !next_cluster || disk >= 4 || fs_info->bytes_per_sector != SECTOR_SIZE || fs_info->sectors_per_cluster == 0 || fs_info->sectors_per_cluster > 128 || fs_info->sector_count == 0 || fs_info->sectors_per_fat == 0 || cluster < 2)
        return false;
    u32 data_start = fs_info->data + 2 * fs_info->sectors_per_cluster;
    if (data_start >= fs_info->sector_count)
        return false;
    u32 max_clusters = (fs_info->sector_count - data_start) / fs_info->sectors_per_cluster;
    if (max_clusters == 0 || cluster - 2 >= max_clusters || cluster / FEPS >= fs_info->sectors_per_fat) {
        diagnostics_log("FAT32: cluster-chain lookup is outside the FAT or data region.");
        return false;
    }
    if (!readsectorpio(partoffset + fs_info->fat + cluster / FEPS, 1, fat_buffer, disk)) {
        diagnostics_log("FAT32: FAT-sector read failed while following a cluster chain.");
        return false;
    }
    u32 next = fat_buffer[cluster % FEPS] & 0x0FFFFFFF;
    if (next < 2 || next >= 0x0FFFFFF0 || next - 2 >= max_clusters) {
        diagnostics_log("FAT32: cluster chain points outside the valid data region.");
        return false;
    }
    *next_cluster = next;
    return true;
}
size_t fat_read(void *ptr, size_t bytes, FILE *fp) {
    if (!ptr || !fp || !fp->path || !volumes || !disktable || fp->volume >= LETTERS || bytes == 0)
        return 0;
    unsigned int disk = volumes[fp->volume].disk;
    unsigned int part = volumes[fp->volume].partition;
    if (disk >= 4 || part >= 4)
        return 0;
    fat_fs_info *fs_info = (fat_fs_info *)disktable[disk].parts[part].fsdata;
    if (!fs_info || fs_info->bytes_per_sector != SECTOR_SIZE || fs_info->sectors_per_cluster == 0 || fs_info->sectors_per_cluster > 128 || (fs_info->sectors_per_cluster & (fs_info->sectors_per_cluster - 1)) != 0 || fs_info->sectors_per_fat == 0 || fs_info->sector_count == 0) {
        diagnostics_log("FAT32 read: filesystem metadata is unavailable or invalid.");
        return 0;
    }
    if (fp->pointer >= fp->size)
        return 0;
    if (bytes > fp->size - fp->pointer)
        bytes = fp->size - fp->pointer;
    FAT_entry entry;
    char filename[256];
    if (find_fat_entry(&entry, filename, fp) != 0) {
        diagnostics_log("FAT32 read: file entry lookup failed.");
        return 0;
    }
    if (bytes == 0 || entry.size == 0)
        return 0;
    u32 cluster = ((u32)entry.cluster_high << 16) | entry.cluster_low;
    u32 bytes_per_cluster = (u32)fs_info->bytes_per_sector * fs_info->sectors_per_cluster;
    u32 data_start = fs_info->data + 2 * fs_info->sectors_per_cluster;
    if (data_start >= fs_info->sector_count) {
        diagnostics_log("FAT32 read: data region is outside the volume bounds.");
        return 0;
    }
    u32 max_clusters = (fs_info->sector_count - data_start) / fs_info->sectors_per_cluster;
    if (max_clusters == 0 || (u64)max_clusters + 2 > (u64)fs_info->sectors_per_fat * FEPS) {
        diagnostics_log("FAT32 read: cluster count exceeds the FAT or data region capacity.");
        return 0;
    }
    if (cluster < 2 || cluster - 2 >= max_clusters || bytes_per_cluster == 0) {
        diagnostics_log("FAT32 read: starting cluster is invalid.");
        return 0;
    }
    u32 skipped = fp->pointer / bytes_per_cluster;
    for (u32 i = 0; i < skipped; ++i) {
        u32 next = 0;
        if (!fat32_next_cluster(disktable[disk].parts[part].loc, fs_info, cluster, disk, &next))
            return 0;
        if (next < 2 || next == 0x0FFFFFF7 || next >= 0x0FFFFFF8 || next - 2 >= max_clusters) {
            diagnostics_log("FAT32 read: cluster chain ended before the requested file offset.");
            return 0;
        }
        cluster = next;
    }
    u8 *buffer = malloc(bytes_per_cluster);
    if (!buffer) {
        diagnostics_log("FAT32 read: cluster buffer allocation failed.");
        return 0;
    }
    size_t copied = 0;
    size_t offset = fp->pointer % bytes_per_cluster;
    u64 partoffset = disktable[disk].parts[part].loc;
    while (copied < bytes) {
        if (cluster < 2 || cluster - 2 >= max_clusters) {
            free(buffer);
            diagnostics_log("FAT32 read: cluster chain contains an invalid cluster.");
            fp->pointer += copied;
            return copied;
        }
        u64 data_lba = partoffset + fs_info->data + (u64)cluster * fs_info->sectors_per_cluster;
        if (!readsectorpio(data_lba, fs_info->sectors_per_cluster, buffer, disk)) {
            free(buffer);
            diagnostics_log("FAT32 read: data-cluster read failed.");
            fp->pointer += copied;
            return copied;
        }
        size_t available = bytes_per_cluster - offset;
        size_t take = bytes - copied;
        if (take > available)
            take = available;
        memcpy((u8 *)ptr + copied, buffer + offset, take);
        copied += take;
        offset = 0;
        if (copied < bytes) {
            u32 next = 0;
            if (!fat32_next_cluster(partoffset, fs_info, cluster, disk, &next)) {
                free(buffer);
                fp->pointer += copied;
                return copied;
            }
            if (next < 2 || next == 0x0FFFFFF7 || next >= 0x0FFFFFF8 || next - 2 >= max_clusters) {
                free(buffer);
                diagnostics_log("FAT32 read: cluster chain ended before the requested data was read.");
                fp->pointer += copied;
                return copied;
            }
            cluster = next;
        }
    }
    free(buffer);
    fp->pointer += copied;
    return copied;
}

int find_fat_entry(FAT_entry *entry, char filename[256], const FILE *fp) {
    if (!entry || !filename || !fp || !fp->path || !volumes || !disktable || fp->volume >= LETTERS)
        return 1;
    if (fp->path_count == 0 || fp->path_count >= 16)
        return 1;
    for (u32 path_index = 0; path_index < fp->path_count; ++path_index) {
        if (!fp->path[path_index])
            return 1;
    }
    if (fp->path[fp->path_count])
        return 1;
    unsigned int disk = volumes[fp->volume].disk;
    unsigned int part = volumes[fp->volume].partition;
    if (disk >= 4 || part >= 4)
        return 1;
    LBA partoffset = disktable[disk].parts[part].loc;
    fat_fs_info *fs_info = (fat_fs_info *)disktable[disk].parts[part].fsdata;
    if (!fs_info || fs_info->bytes_per_sector != SECTOR_SIZE || fs_info->sectors_per_cluster == 0 || fs_info->sectors_per_cluster > 128 || fs_info->sector_count == 0 || !fp->path[0])
        return 1;
    unsigned int i, j, len = 0;
    char charbuf[6];
    unsigned int depth = 0;
    u32 active_cluster = fs_info->root_cluster;
    u32 *FAT_buffer = malloc(SECTOR_SIZE);
    const u32 bytes_per_cluster = fs_info->bytes_per_sector * fs_info->sectors_per_cluster;
    FAT_entry *buffer = malloc(bytes_per_cluster);
    if (!FAT_buffer || !buffer) {
        free(FAT_buffer);
        free(buffer);
        diagnostics_log("FAT32 lookup: memory allocation failed.");
        return 1;
    }
    FAT_entry *current;
    int result = 2;
    u32 traversed = 0;
    u32 data_start = fs_info->data + 2 * fs_info->sectors_per_cluster;
    if (data_start >= fs_info->sector_count) {
        diagnostics_log("FAT32 lookup: data region is outside the volume bounds.");
        result = 1;
        goto cleanup;
    }
    u32 traversal_limit = (fs_info->sector_count - data_start) / fs_info->sectors_per_cluster;
    if (traversal_limit == 0 || (u64)traversal_limit + 2 > (u64)fs_info->sectors_per_fat * FEPS) {
        diagnostics_log("FAT32 lookup: cluster count exceeds the FAT or data region capacity.");
        result = 1;
        goto cleanup;
    }
    if (!readsectorpio(partoffset + fs_info->fat + active_cluster / FEPS, 1, FAT_buffer, disk)) {
        diagnostics_log("FAT32 lookup: root FAT-sector read failed.");
        result = 1;
        goto cleanup;
    }

	do {
inloop:
        if (++traversed > traversal_limit || active_cluster < 2 || active_cluster - 2 >= traversal_limit) {
            diagnostics_log("FAT32 lookup: directory chain exceeded its safe traversal limit.");
            result = 1;
            goto cleanup;
        }
        if (!readsectorpio(partoffset + fs_info->data + active_cluster * fs_info->sectors_per_cluster, fs_info->sectors_per_cluster, buffer, disk)) {
            diagnostics_log("FAT32 lookup: directory data read failed.");
            result = 1;
            goto cleanup;
        }
inloop2:
		current = buffer;

		/* Under this, everything to right of '<' is the amount of fat entries in a cluster */
		for (i = 0; i < bytes_per_cluster / sizeof(FAT_entry); ++i) {
			if (current->attr == LFN) {
				while (current->attr == LFN) {
					u32 order_index = ((LFN_entry *)current)->order & 0x1F;
                    if (order_index == 0 || order_index > 20) {
                        diagnostics_log("FAT32 lookup: invalid long-filename entry order.");
                        result = 1;
                        goto cleanup;
                    }
                    u32 filename_offset = (order_index - 1) * 13;
                    u32 room = sizeof(filename) - 1 - filename_offset;
                    u32 count;
                    wcatomba((u8 *)charbuf, ((LFN_entry *)current)->first, 5);
                    count = room < 5 ? room : 5;
                    if (count)
                        memcpy(filename + filename_offset, charbuf, count);
                    room -= count;
                    wcatomba((u8 *)charbuf, ((LFN_entry *)current)->middle, 6);
                    count = room < 6 ? room : 6;
                    if (count)
                        memcpy(filename + filename_offset + 5, charbuf, count);
                    room -= count;
                    wcatomba((u8 *)charbuf, ((LFN_entry *)current)->last, 2);
                    count = room < 2 ? room : 2;
                    if (count)
                        memcpy(filename + filename_offset + 11, charbuf, count);
                    if (len <= sizeof(filename) - 14)
                        len += 13;
                    else
                        len = sizeof(filename) - 1;
					++current;
					++i;

					if (i >= bytes_per_cluster / sizeof(FAT_entry)) {
                    if (!readsectorpio(partoffset + fs_info->fat + FAT_buffer[active_cluster % FEPS] / FEPS, 1, FAT_buffer, disk)) {
                        diagnostics_log("FAT32 lookup: FAT-sector read failed while scanning a directory.");
                        result = 1;
                        goto cleanup;
                    }
                    active_cluster = FAT_buffer[active_cluster % FEPS] & 0x0FFFFFFF;
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

			if (depth >= fp->path_count || !fp->path[depth]) {
                    diagnostics_log("FAT32 lookup: path depth exceeded its validated component count.");
                    result = 1;
                    goto cleanup;
                }
                if (!strcmp(filename, fp->path[depth])) {
                u32 matched_cluster = ((u32)current->cluster_high << 16) | current->cluster_low;
                bool matched_directory = (current->attr & 0x10) != 0;
                if ((matched_directory || current->size != 0) && (matched_cluster < 2 || matched_cluster - 2 >= traversal_limit)) {
                    diagnostics_log("FAT32 lookup: matched entry points outside the valid cluster range.");
                    result = 1;
                    goto cleanup;
                }
                active_cluster = matched_cluster;
                memcpy(entry, current, sizeof(FAT_entry));
                if (!(current->attr & 0x10)) {
                    result = 0;
                    goto cleanup;
                }
                ++depth;
                if (!fp->path[depth]) {
                    result = 2;
                    goto cleanup;
                }
                goto inloop;
            }
			++current;
		}
        if (!readsectorpio(partoffset + fs_info->fat + FAT_buffer[active_cluster % FEPS] / FEPS, 1, FAT_buffer, disk)) {
            diagnostics_log("FAT32 lookup: FAT-sector read failed while advancing a directory chain.");
            result = 1;
            goto cleanup;
        }
        active_cluster = FAT_buffer[active_cluster % FEPS] & 0x0FFFFFFF;
	} while ((active_cluster & 0x0FFFFFFF) < 0x0FFFFFF0);

    result = 2;
cleanup:
    free(FAT_buffer);
    free(buffer);
    return result;
}

bool fat_filesize(FILE *fp, unsigned long long *size) {
    if (!fp || !size)
        return false;
    FAT_entry entry;
    char filename[256];
    if (find_fat_entry(&entry, filename, fp) != 0) {
        diagnostics_log("FAT32 file-size lookup failed.");
        return false;
    }
    *size = entry.size;
    return true;
}

int fat_detect(unsigned int disk) {
    if (disk >= 4)
        return 0;
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

    u32 volume_sectors = (u32)boot[19] | ((u32)boot[20] << 8);
    if (volume_sectors == 0)
        volume_sectors = (u32)boot[32] | ((u32)boot[33] << 8) | ((u32)boot[34] << 16) | ((u32)boot[35] << 24);
    u64 partition_end = (u64)partition_lba + volume_sectors;
    if (sectors_per_cluster > 128 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 || volume_sectors == 0 || partition_end > 0x100000000ULL || (disktable && disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
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

static bool fat16_next_cluster(u32 fat_lba, u16 cluster, unsigned int disk, u16 *next) {
    u8 sector[512];
    if (!next || disk >= 4 || cluster < 2) {
        diagnostics_log("FAT16: invalid cluster-chain lookup request.");
        return false;
    }
    u32 offset = (u32)cluster * 2;
    u32 lba = fat_lba + offset / 512;
    u32 pos = offset % 512;
    if (!disk_read_sector(lba, sector, disk)) {
        diagnostics_log("FAT16: FAT-sector read failed while following a cluster chain.");
        return false;
    }
    *next = (u16)sector[pos] | ((u16)sector[pos + 1] << 8);
    return true;
}

static bool fat16_find_in_dir(
    unsigned int disk,
    u32 root_lba,
    u32 root_sectors,
    u32 data_lba,
    u8 sectors_per_cluster,
    u32 fat_lba,
    u32 cluster_limit,
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
    u32 traversed = 0;
    if (cluster_limit == 0 || cluster < 2 || cluster >= 0xFFF0 || (u32)cluster - 2 >= cluster_limit)
        return false;

    while (cluster >= 2 && cluster < 0xFFF0 && (u32)cluster - 2 < cluster_limit && traversed < cluster_limit) {
        ++traversed;
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

        u16 next;
        if (!fat16_next_cluster(fat_lba, cluster, disk, &next))
            return false;

        if (next < 2 || next >= 0xFFF0 || (u32)next - 2 >= cluster_limit)
            return false;

        cluster = next;
    }

    return false;
}

int fat16_find_file_entry(const FILE *fp, FAT_entry *entry) {
    if (!fp || !entry || !fp->path || !volumes || !disktable || fp->volume >= LETTERS)
        return 0;

    unsigned int disk = volumes[fp->volume].disk;
    unsigned int part = volumes[fp->volume].partition;
    if (disk >= 4 || part >= 4 || disktable[disk].parts[part].fs != fat16)
        return 0;

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

    u32 total_sectors =
        (u32)boot[19] |
        ((u32)boot[20] << 8);

    if (total_sectors == 0) {
        total_sectors =
            (u32)boot[32] |
            ((u32)boot[33] << 8) |
            ((u32)boot[34] << 16) |
            ((u32)boot[35] << 24);
    }

    u64 root_sectors_wide = ((u64)root_entries * 32 + 511) / 512;
    u64 metadata_sectors = (u64)reserved_sectors + (u64)fat_count * sectors_per_fat + root_sectors_wide;
    u64 partition_end = (u64)partition_lba + total_sectors;

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0 ||
        total_sectors == 0 ||
        metadata_sectors >= total_sectors ||
        partition_end > 0x100000000ULL ||
        (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
        return 0;

    u32 root_sectors = (u32)root_sectors_wide;
    u32 cluster_count = (total_sectors - (u32)metadata_sectors) / sectors_per_cluster;
    if (cluster_count == 0 || cluster_count > 0xFFEE || (u64)cluster_count + 2 > (u64)sectors_per_fat * 256)
        return 0;

    u32 fat_lba = partition_lba + reserved_sectors;
    u32 root_lba = fat_lba + (u32)fat_count * sectors_per_fat;
    u32 data_lba = root_lba + root_sectors;

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
                cluster_count,
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

static bool fat16_write_fat_entry_copy(
    u32 fat_base,
    u32 sectors_per_fat,
    u16 cluster,
    u16 value,
    unsigned int disk
) {
    u32 offset = (u32)cluster * 2;

    if (offset + 1 >= sectors_per_fat * 512)
        return false;

    u32 lba = fat_base + offset / 512;
    u32 pos = offset % 512;

    u8 sector[512];
    u8 verify[512];

    if (!disk_read_sector(lba, sector, disk))
        return false;

    sector[pos] = (u8)value;
    sector[pos + 1] = (u8)(value >> 8);

    if (!disk_write_sector(lba, sector, disk))
        return false;

    if (!disk_read_sector(lba, verify, disk))
        return false;

    return verify[pos] == (u8)value &&
           verify[pos + 1] == (u8)(value >> 8);
}

static bool fat16_clear_cluster(u32 fat_lba, u32 sectors_per_fat, u8 fat_count, u16 cluster, unsigned int disk) {
    for (u32 copy = 0; copy < fat_count; ++copy) {
        if (!fat16_write_fat_entry_copy(fat_lba + copy * sectors_per_fat, sectors_per_fat, cluster, 0, disk)) {
            diagnostics_log("FAT16: failed to release a cluster in every FAT copy.");
            return false;
        }
    }
    return true;
}

int fat16_write_empty_file(
    const char name[11],
    const u8 *data,
    u32 length
) {
    if (!name || !data || length == 0)
        return FAT16_WRITE_BAD_FS;

    if (!disktable || !volumes ||
        disktable[0].parts[0].fs != fat16)
        return FAT16_WRITE_BAD_FS;

    unsigned int disk = volumes[0].disk;

    if (disk >= 4)
        return FAT16_WRITE_BAD_FS;
    u8 mbr[512];
    u8 boot[512];
    u8 sector[512];
    u8 verify[512];

    if (!disk_read_sector(0, mbr, disk))
        return FAT16_WRITE_IO_ERROR;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return FAT16_WRITE_BAD_FS;

    u32 partition_lba =
        (u32)mbr[446 + 8] |
        ((u32)mbr[446 + 9] << 8) |
        ((u32)mbr[446 + 10] << 16) |
        ((u32)mbr[446 + 11] << 24);

    if (!disk_read_sector(partition_lba, boot, disk))
        return FAT16_WRITE_IO_ERROR;

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

    u32 total_sectors =
        (u16)boot[19] |
        ((u32)boot[20] << 8);

    if (total_sectors == 0) {
        total_sectors =
            (u32)boot[32] |
            ((u32)boot[33] << 8) |
            ((u32)boot[34] << 16) |
            ((u32)boot[35] << 24);
    }
    u64 partition_end = (u64)partition_lba + total_sectors;
    if (partition_lba == 0 || total_sectors == 0 || partition_end > 0x100000000ULL || (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
        return FAT16_WRITE_BAD_FS;

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0 ||
        total_sectors == 0)
        return FAT16_WRITE_BAD_FS;

    u32 root_sectors =
        ((u32)root_entries * 32 + 511) / 512;

    u32 fat_lba =
        partition_lba + reserved_sectors;

    u32 root_lba =
        fat_lba + (u32)fat_count * sectors_per_fat;

    u32 metadata_sectors =
        (u32)reserved_sectors +
        (u32)fat_count * sectors_per_fat +
        root_sectors;

    if (total_sectors <= metadata_sectors)
        return FAT16_WRITE_BAD_FS;

    u32 data_lba =
        partition_lba + metadata_sectors;

    u32 cluster_count =
        (total_sectors - metadata_sectors) /
        sectors_per_cluster;

    u32 max_cluster = cluster_count + 1;

    if (max_cluster > 0xFFEF)
        max_cluster = 0xFFEF;

    u32 fat_entry_count =
        (u32)sectors_per_fat * 512 / 2;

    if (fat_entry_count < 3)
        return FAT16_WRITE_BAD_FS;

    if (max_cluster >= fat_entry_count)
        max_cluster = fat_entry_count - 1;

    if (max_cluster < 2)
        return FAT16_WRITE_BAD_FS;

    u32 bytes_per_cluster =
        (u32)sectors_per_cluster * 512;

    if (length > bytes_per_cluster)
        return FAT16_WRITE_TOO_LARGE;

    bool found = false;
    bool end_of_directory = false;

    u32 directory_sector_lba = 0;
    u32 directory_offset = 0;
    u8 directory_entry[32];

    u32 entries_seen = 0;

    for (u32 s = 0;
         s < root_sectors && !found && !end_of_directory;
         ++s) {

        if (!disk_read_sector(root_lba + s, sector, disk))
            return FAT16_WRITE_IO_ERROR;

        for (u32 off = 0;
             off < 512 && entries_seen < root_entries;
             off += 32, ++entries_seen) {

            u8 first = sector[off];

            if (first == 0x00) {
                end_of_directory = true;
                break;
            }

            if (first == 0xE5)
                continue;

            u8 attr = sector[off + 11];

            if (attr == 0x0F || (attr & 0x08))
                continue;

            bool same = true;

            for (u32 i = 0; i < 11; ++i) {
                if (sector[off + i] != (u8)name[i]) {
                    same = false;
                    break;
                }
            }

            if (same) {
                memcpy(
                    directory_entry,
                    &sector[off],
                    sizeof(directory_entry)
                );

                directory_sector_lba = root_lba + s;
                directory_offset = off;
                found = true;
                break;
            }
        }
    }

    if (!found)
        return FAT16_WRITE_NOT_FOUND;

    if (directory_entry[11] & 0x10)
        return FAT16_WRITE_NOT_FILE;

    u16 old_cluster =
        (u16)directory_entry[26] |
        ((u16)directory_entry[27] << 8);

    u16 old_cluster_high =
        (u16)directory_entry[20] |
        ((u16)directory_entry[21] << 8);

    u32 old_size =
        (u32)directory_entry[28] |
        ((u32)directory_entry[29] << 8) |
        ((u32)directory_entry[30] << 16) |
        ((u32)directory_entry[31] << 24);

    if (old_size != 0 ||
        old_cluster != 0 ||
        old_cluster_high != 0)
        return FAT16_WRITE_NOT_EMPTY;

    u16 free_cluster = 0;

    for (u32 c = 2; c <= max_cluster; ++c) {
        bool free_cluster_candidate = true;

        for (u32 copy = 0; copy < fat_count; ++copy) {
            u32 offset = c * 2;

            u32 lba =
                fat_lba +
                copy * sectors_per_fat +
                offset / 512;

            u32 pos = offset % 512;

            if (!disk_read_sector(lba, sector, disk))
                return FAT16_WRITE_IO_ERROR;

            u16 value =
                (u16)sector[pos] |
                ((u16)sector[pos + 1] << 8);

            if (value != 0) {
                free_cluster_candidate = false;
                break;
            }
        }

        if (free_cluster_candidate) {
            free_cluster = (u16)c;
            break;
        }
    }

    if (free_cluster == 0)
        return FAT16_WRITE_NO_SPACE;

    u32 cluster_lba =
        data_lba +
        ((u32)free_cluster - 2) * sectors_per_cluster;

    for (u32 s = 0; s < sectors_per_cluster; ++s) {
        memset(sector, 0, sizeof(sector));

        u32 start = s * 512;

        if (start < length) {
            u32 count = length - start;

            if (count > 512)
                count = 512;

            memcpy(sector, data + start, count);
        }

        if (!disk_write_sector(
                cluster_lba + s,
                sector,
                disk))
            return FAT16_WRITE_IO_ERROR;

        if (!disk_read_sector(
                cluster_lba + s,
                verify,
                disk))
            return FAT16_WRITE_IO_ERROR;

        for (u32 i = 0; i < 512; ++i) {
            if (sector[i] != verify[i])
                return FAT16_WRITE_IO_ERROR;
        }
    }

    for (u32 copy = 0; copy < fat_count; ++copy) {
        if (!fat16_write_fat_entry_copy(
                fat_lba + copy * sectors_per_fat,
                sectors_per_fat,
                free_cluster,
                0xFFFF,
                disk)) {

            fat16_clear_cluster(
                fat_lba,
                sectors_per_fat,
                fat_count,
                free_cluster,
                disk
            );

            return FAT16_WRITE_IO_ERROR;
        }
    }

    if (!disk_read_sector(
            directory_sector_lba,
            sector,
            disk)) {

        fat16_clear_cluster(
            fat_lba,
            sectors_per_fat,
            fat_count,
            free_cluster,
            disk
        );

        return FAT16_WRITE_IO_ERROR;
    }

    bool same_name = true;

    for (u32 i = 0; i < 11; ++i) {
        if (sector[directory_offset + i] != (u8)name[i]) {
            same_name = false;
            break;
        }
    }

    if (!same_name ||
        sector[directory_offset + 28] != 0 ||
        sector[directory_offset + 29] != 0 ||
        sector[directory_offset + 30] != 0 ||
        sector[directory_offset + 31] != 0 ||
        sector[directory_offset + 26] != 0 ||
        sector[directory_offset + 27] != 0) {

        fat16_clear_cluster(
            fat_lba,
            sectors_per_fat,
            fat_count,
            free_cluster,
            disk
        );

        return same_name
            ? FAT16_WRITE_NOT_EMPTY
            : FAT16_WRITE_NOT_FOUND;
    }

    sector[directory_offset + 20] = 0;
    sector[directory_offset + 21] = 0;

    sector[directory_offset + 26] = (u8)free_cluster;
    sector[directory_offset + 27] = (u8)(free_cluster >> 8);

    sector[directory_offset + 28] = (u8)length;
    sector[directory_offset + 29] = (u8)(length >> 8);
    sector[directory_offset + 30] = (u8)(length >> 16);
    sector[directory_offset + 31] = (u8)(length >> 24);

    if (!disk_write_sector(
            directory_sector_lba,
            sector,
            disk))
        return FAT16_WRITE_IO_ERROR;




    if (!disk_read_sector(
            directory_sector_lba,
            verify,
            disk))
        return FAT16_WRITE_IO_ERROR;

    bool valid = true;

    for (u32 i = 0; i < 11; ++i) {
        if (verify[directory_offset + i] != (u8)name[i]) {
            valid = false;
            break;
        }
    }

    u32 verify_size =
        (u32)verify[directory_offset + 28] |
        ((u32)verify[directory_offset + 29] << 8) |
        ((u32)verify[directory_offset + 30] << 16) |
        ((u32)verify[directory_offset + 31] << 24);

    u16 verify_cluster =
        (u16)verify[directory_offset + 26] |
        ((u16)verify[directory_offset + 27] << 8);

    if (!valid ||
        verify_size != length ||
        verify_cluster != free_cluster)
        return FAT16_WRITE_IO_ERROR;

    return FAT16_WRITE_OK;
}

int fat16_replace_file(
    const char name[11],
    const u8 *data,
    u32 length
) {
    if (!name || (length != 0 && !data))
        return FAT16_WRITE_BAD_FS;

    if (!disktable || !volumes ||
        disktable[0].parts[0].fs != fat16)
        return FAT16_WRITE_BAD_FS;

    unsigned int disk = volumes[0].disk;

    if (disk >= 4)
        return FAT16_WRITE_BAD_FS;
    if (fat_detect(disk) != 16)
        return FAT16_WRITE_BAD_FS;

    u8 mbr[512];
    u8 boot[512];
    u8 sector[512];
    u8 verify[512];

    if (!disk_read_sector(0, mbr, disk))
        return FAT16_WRITE_IO_ERROR;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return FAT16_WRITE_BAD_FS;

    u32 partition_lba =
        (u32)mbr[446 + 8] |
        ((u32)mbr[446 + 9] << 8) |
        ((u32)mbr[446 + 10] << 16) |
        ((u32)mbr[446 + 11] << 24);

    if (!disk_read_sector(partition_lba, boot, disk))
        return FAT16_WRITE_IO_ERROR;

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

    u32 total_sectors =
        (u16)boot[19] |
        ((u32)boot[20] << 8);

    if (total_sectors == 0) {
        total_sectors =
            (u32)boot[32] |
            ((u32)boot[33] << 8) |
            ((u32)boot[34] << 16) |
            ((u32)boot[35] << 24);
    }
    u64 partition_end = (u64)partition_lba + total_sectors;
    if (partition_lba == 0 || total_sectors == 0 || partition_end > 0x100000000ULL || (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
        return FAT16_WRITE_BAD_FS;

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0 ||
        total_sectors == 0)
        return FAT16_WRITE_BAD_FS;

    u32 root_sectors =
        ((u32)root_entries * 32 + 511) / 512;

    u32 fat_lba =
        partition_lba + reserved_sectors;

    u32 root_lba =
        fat_lba + (u32)fat_count * sectors_per_fat;

    u32 metadata_sectors =
        (u32)reserved_sectors +
        (u32)fat_count * sectors_per_fat +
        root_sectors;

    if (total_sectors <= metadata_sectors)
        return FAT16_WRITE_BAD_FS;

    u32 data_lba =
        partition_lba + metadata_sectors;

    u32 cluster_count =
        (total_sectors - metadata_sectors) /
        sectors_per_cluster;

    u32 max_cluster = cluster_count + 1;

    if (max_cluster > 0xFFEF)
        max_cluster = 0xFFEF;

    u32 fat_entry_count =
        (u32)sectors_per_fat * 512 / 2;

    if (fat_entry_count < 3)
        return FAT16_WRITE_BAD_FS;

    if (max_cluster >= fat_entry_count)
        max_cluster = fat_entry_count - 1;

    if (max_cluster < 2)
        return FAT16_WRITE_BAD_FS;

    u32 bytes_per_cluster =
        (u32)sectors_per_cluster * 512;

    u32 required_clusters =
        length / bytes_per_cluster;

    if (length % bytes_per_cluster)
        ++required_clusters;

    if (required_clusters > max_cluster - 1)
        return FAT16_WRITE_NO_SPACE;

    u16 *new_clusters = 0;

    if (required_clusters != 0) {
        new_clusters = malloc(
            required_clusters * sizeof(u16)
        );

        if (!new_clusters)
            return FAT16_WRITE_NO_MEMORY;
    }

    int failure_result = FAT16_WRITE_IO_ERROR;

    bool found = false;
    bool end_of_directory = false;

    u32 directory_sector_lba = 0;
    u32 directory_offset = 0;
    u8 directory_entry[32];

    u32 entries_seen = 0;

    for (u32 s = 0;
         s < root_sectors && !found && !end_of_directory;
         ++s) {

        if (!disk_read_sector(root_lba + s, sector, disk)) {
            free(new_clusters);
            return FAT16_WRITE_IO_ERROR;
        }

        for (u32 off = 0;
             off < 512 && entries_seen < root_entries;
             off += 32, ++entries_seen) {

            u8 first = sector[off];

            if (first == 0x00) {
                end_of_directory = true;
                break;
            }

            if (first == 0xE5)
                continue;

            u8 attr = sector[off + 11];

            if (attr == 0x0F || (attr & 0x08))
                continue;

            bool same = true;

            for (u32 i = 0; i < 11; ++i) {
                if (sector[off + i] != (u8)name[i]) {
                    same = false;
                    break;
                }
            }

            if (same) {
                memcpy(
                    directory_entry,
                    &sector[off],
                    sizeof(directory_entry)
                );

                directory_sector_lba = root_lba + s;
                directory_offset = off;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        free(new_clusters);
        return FAT16_WRITE_NOT_FOUND;
    }

    if (directory_entry[11] & 0x10) {
        free(new_clusters);
        return FAT16_WRITE_NOT_FILE;
    }

    u16 old_cluster =
        (u16)directory_entry[26] |
        ((u16)directory_entry[27] << 8);

    u16 old_cluster_high =
        (u16)directory_entry[20] |
        ((u16)directory_entry[21] << 8);

    u32 old_size =
        (u32)directory_entry[28] |
        ((u32)directory_entry[29] << 8) |
        ((u32)directory_entry[30] << 16) |
        ((u32)directory_entry[31] << 24);

    if (old_cluster_high != 0 ||
        (old_size != 0 && old_cluster < 2)) {

        free(new_clusters);
        return FAT16_WRITE_BAD_FS;
    }

    if (old_cluster == 0 &&
        old_size == 0 &&
        length > 0 &&
        length <= bytes_per_cluster) {
        free(new_clusters);
        return fat16_write_empty_file(name, data, length);
    }

    if (old_cluster >= 2 &&
        old_cluster <= max_cluster &&
        old_size <= bytes_per_cluster &&
        length <= bytes_per_cluster) {

        u32 old_fat_offset = (u32)old_cluster * 2;
        u8 old_fat_sector[512];

        if (!disk_read_sector(
                fat_lba + old_fat_offset / 512,
                old_fat_sector,
                disk)) {
            free(new_clusters);
            return FAT16_WRITE_IO_ERROR;
        }

        u32 old_fat_pos = old_fat_offset % 512;
        u16 old_next =
            (u16)old_fat_sector[old_fat_pos] |
            ((u16)old_fat_sector[old_fat_pos + 1] << 8);

        if (old_next >= 0xFFF8) {
            if (!disk_read_sector(
                    directory_sector_lba,
                    sector,
                    disk)) {
                free(new_clusters);
                return FAT16_WRITE_IO_ERROR;
            }

            bool original_name_ok = true;
            for (u32 i = 0; i < 11; ++i) {
                if (sector[directory_offset + i] != (u8)name[i]) {
                    original_name_ok = false;
                    break;
                }
            }

            u16 original_cluster =
                (u16)sector[directory_offset + 26] |
                ((u16)sector[directory_offset + 27] << 8);
            u32 original_size =
                (u32)sector[directory_offset + 28] |
                ((u32)sector[directory_offset + 29] << 8) |
                ((u32)sector[directory_offset + 30] << 16) |
                ((u32)sector[directory_offset + 31] << 24);

            if (!original_name_ok ||
                original_cluster != old_cluster ||
                original_size != old_size) {
                free(new_clusters);
                return FAT16_WRITE_NOT_EMPTY;
            }

            u32 existing_cluster_lba =
                data_lba +
                ((u32)old_cluster - 2) * sectors_per_cluster;
            u32 data_offset = 0;

            for (u32 s = 0; s < sectors_per_cluster; ++s) {
                memset(sector, 0, sizeof(sector));
                u32 count = 0;

                if (data_offset < length) {
                    count = length - data_offset;
                    if (count > 512)
                        count = 512;
                    if (count != 0)
                        memcpy(sector, data + data_offset, count);
                }

                if (!disk_write_sector(
                        existing_cluster_lba + s,
                        sector,
                        disk)) {
                    free(new_clusters);
                    return FAT16_WRITE_IO_ERROR;
                }

                if (!disk_read_sector(
                        existing_cluster_lba + s,
                        verify,
                        disk)) {
                    free(new_clusters);
                    return FAT16_WRITE_IO_ERROR;
                }

                for (u32 b = 0; b < 512; ++b) {
                    if (sector[b] != verify[b]) {
                        free(new_clusters);
                        return FAT16_WRITE_IO_ERROR;
                    }
                }

                data_offset += count;
            }

            if (!disk_read_sector(
                    directory_sector_lba,
                    sector,
                    disk)) {
                free(new_clusters);
                return FAT16_WRITE_IO_ERROR;
            }

            bool current_name_ok = true;
            for (u32 i = 0; i < 11; ++i) {
                if (sector[directory_offset + i] != (u8)name[i]) {
                    current_name_ok = false;
                    break;
                }
            }

            u16 current_cluster =
                (u16)sector[directory_offset + 26] |
                ((u16)sector[directory_offset + 27] << 8);
            u32 current_size =
                (u32)sector[directory_offset + 28] |
                ((u32)sector[directory_offset + 29] << 8) |
                ((u32)sector[directory_offset + 30] << 16) |
                ((u32)sector[directory_offset + 31] << 24);

            if (!current_name_ok ||
                current_cluster != old_cluster ||
                current_size != old_size) {
                free(new_clusters);
                return FAT16_WRITE_NOT_EMPTY;
            }

            sector[directory_offset + 20] = 0;
            sector[directory_offset + 21] = 0;
            sector[directory_offset + 26] = (u8)old_cluster;
            sector[directory_offset + 27] = (u8)(old_cluster >> 8);
            sector[directory_offset + 28] = (u8)length;
            sector[directory_offset + 29] = (u8)(length >> 8);
            sector[directory_offset + 30] = (u8)(length >> 16);
            sector[directory_offset + 31] = (u8)(length >> 24);

            if (!disk_write_sector(
                    directory_sector_lba,
                    sector,
                    disk)) {
                free(new_clusters);
                return FAT16_WRITE_IO_ERROR;
            }

            if (!disk_read_sector(
                    directory_sector_lba,
                    verify,
                    disk)) {
                free(new_clusters);
                return FAT16_WRITE_IO_ERROR;
            }

            bool saved_name_ok = true;
            for (u32 i = 0; i < 11; ++i) {
                if (verify[directory_offset + i] != (u8)name[i]) {
                    saved_name_ok = false;
                    break;
                }
            }

            u16 saved_cluster =
                (u16)verify[directory_offset + 26] |
                ((u16)verify[directory_offset + 27] << 8);
            u32 saved_size =
                (u32)verify[directory_offset + 28] |
                ((u32)verify[directory_offset + 29] << 8) |
                ((u32)verify[directory_offset + 30] << 16) |
                ((u32)verify[directory_offset + 31] << 24);

            free(new_clusters);

            if (!saved_name_ok ||
                saved_cluster != old_cluster ||
                saved_size != length)
                return FAT16_WRITE_IO_ERROR;

            return FAT16_WRITE_OK;
        }
    }

    u8 *mirror_fat_sectors = 0;

    if (fat_count > 1) {
        size_t mirror_bytes = (size_t)(fat_count - 1) * 512;
        mirror_fat_sectors = malloc(mirror_bytes);

        if (!mirror_fat_sectors) {
            free(new_clusters);
            return FAT16_WRITE_NO_MEMORY;
        }
    }

    u32 allocated_count = 0;

    for (u32 fat_sector_index = 0;
         fat_sector_index < sectors_per_fat &&
         fat_sector_index <= max_cluster / 256 &&
         allocated_count < required_clusters;
         ++fat_sector_index) {



        if (!disk_read_sector(
                fat_lba + fat_sector_index,
                sector,
                disk)) {
            free(mirror_fat_sectors);
            free(new_clusters);
            return FAT16_WRITE_IO_ERROR;
        }



        for (u32 copy = 1; copy < fat_count; ++copy) {
            u8 *mirror =
                mirror_fat_sectors + (size_t)(copy - 1) * 512;


            if (!disk_read_sector(
                    fat_lba +
                    copy * sectors_per_fat +
                    fat_sector_index,
                    mirror,
                    disk)) {
                free(mirror_fat_sectors);
                free(new_clusters);
                return FAT16_WRITE_IO_ERROR;
            }

        }



        for (u32 entry_index = 0;
             entry_index < 256 &&
             allocated_count < required_clusters;
             ++entry_index) {

            u32 cluster = fat_sector_index * 256 + entry_index;

            if (cluster < 2)
                continue;

            if (cluster > max_cluster)
                break;

            u32 pos = entry_index * 2;

            u16 value =
                (u16)sector[pos] |
                ((u16)sector[pos + 1] << 8);

            if (value != 0)
                continue;


            bool is_free = true;

            for (u32 copy = 1; copy < fat_count; ++copy) {

                const u8 *mirror =
                    mirror_fat_sectors + (size_t)(copy - 1) * 512;

                u16 mirror_value =
                    (u16)mirror[pos] |
                    ((u16)mirror[pos + 1] << 8);

                if (mirror_value != 0) {
                    is_free = false;
                    break;
                }
            }


            if (is_free) {
                new_clusters[allocated_count++] = (u16)cluster;
            }
        }


    }

    free(mirror_fat_sectors);

    if (allocated_count != required_clusters) {
        free(new_clusters);
        return FAT16_WRITE_NO_SPACE;
    }

    u32 data_offset = 0;

    for (u32 i = 0; i < required_clusters; ++i) {
        u32 cluster_lba =
            data_lba +
            ((u32)new_clusters[i] - 2) *
            sectors_per_cluster;

        for (u32 s = 0; s < sectors_per_cluster; ++s) {
            memset(sector, 0, sizeof(sector));

            u32 count = 0;

            if (data_offset < length) {
                count = length - data_offset;

                if (count > 512)
                    count = 512;

                memcpy(
                    sector,
                    data + data_offset,
                    count
                );
            }

            if (!disk_write_sector(
                    cluster_lba + s,
                    sector,
                    disk
                ))
                goto rollback;

            if (!disk_read_sector(
                    cluster_lba + s,
                    verify,
                    disk))
                goto rollback;

            for (u32 b = 0; b < 512; ++b) {
                if (sector[b] != verify[b])
                    goto rollback;
            }

            data_offset += count;
        }
    }

    for (u32 copy = 0; copy < fat_count; ++copy) {
        for (u32 i = 0; i < required_clusters; ++i) {
            u16 next =
                (i + 1 < required_clusters)
                ? new_clusters[i + 1]
                : 0xFFFF;

            if (!fat16_write_fat_entry_copy(
                    fat_lba + copy * sectors_per_fat,
                    sectors_per_fat,
                    new_clusters[i],
                    next,
                    disk))
                goto rollback;
        }
    }

    if (!disk_read_sector(
            directory_sector_lba,
            sector,
            disk))
        goto rollback;

    bool same_name = true;

    for (u32 i = 0; i < 11; ++i) {
        if (sector[directory_offset + i] != (u8)name[i]) {
            same_name = false;
            break;
        }
    }

    u16 current_cluster =
        (u16)sector[directory_offset + 26] |
        ((u16)sector[directory_offset + 27] << 8);

    u32 current_size =
        (u32)sector[directory_offset + 28] |
        ((u32)sector[directory_offset + 29] << 8) |
        ((u32)sector[directory_offset + 30] << 16) |
        ((u32)sector[directory_offset + 31] << 24);

    if (!same_name ||
        current_cluster != old_cluster ||
        current_size != old_size) {

        failure_result = FAT16_WRITE_NOT_EMPTY;
        goto rollback;
    }

    u16 first_cluster =
        required_clusters ? new_clusters[0] : 0;

    sector[directory_offset + 20] = 0;
    sector[directory_offset + 21] = 0;

    sector[directory_offset + 26] = (u8)first_cluster;
    sector[directory_offset + 27] = (u8)(first_cluster >> 8);

    sector[directory_offset + 28] = (u8)length;
    sector[directory_offset + 29] = (u8)(length >> 8);
    sector[directory_offset + 30] = (u8)(length >> 16);
    sector[directory_offset + 31] = (u8)(length >> 24);

    bool directory_write_ok = disk_write_sector(
        directory_sector_lba,
        sector,
        disk
    );


    if (!disk_read_sector(
            directory_sector_lba,
            verify,
            disk)) {

        free(new_clusters);
        return FAT16_WRITE_IO_ERROR;
    }

    bool new_entry_valid = true;

    for (u32 i = 0; i < 11; ++i) {
        if (verify[directory_offset + i] != (u8)name[i]) {
            new_entry_valid = false;
            break;
        }
    }

    u16 verify_cluster =
        (u16)verify[directory_offset + 26] |
        ((u16)verify[directory_offset + 27] << 8);

    u32 verify_size =
        (u32)verify[directory_offset + 28] |
        ((u32)verify[directory_offset + 29] << 8) |
        ((u32)verify[directory_offset + 30] << 16) |
        ((u32)verify[directory_offset + 31] << 24);

    if (!new_entry_valid ||
        verify_cluster != first_cluster ||
        verify_size != length) {

        if (!directory_write_ok &&
            new_entry_valid &&
            verify_cluster == old_cluster &&
            verify_size == old_size) {
            goto rollback;
        }

        free(new_clusters);
        return FAT16_WRITE_IO_ERROR;
    }

    u16 old_current = old_cluster;

    for (u32 step = 0;
         old_current >= 2 &&
         old_current <= max_cluster &&
         step < max_cluster;
         ++step) {

        u16 next;
        if (!fat16_next_cluster(fat_lba, old_current, disk, &next)) {
            free(new_clusters);
            diagnostics_log("FAT16 replace: old cluster chain could not be read during cleanup.");
            return FAT16_WRITE_DELETE_CLEANUP_ERROR;
        }
        if (!fat16_clear_cluster(fat_lba, sectors_per_fat, fat_count, old_current, disk)) {
            free(new_clusters);
            diagnostics_log("FAT16 replace: old cluster cleanup failed after committing the new file.");
            return FAT16_WRITE_DELETE_CLEANUP_ERROR;
        }

        if (next < 2 ||
            next == 0xFFF7 ||
            next >= 0xFFF8 ||
            next > max_cluster)
            break;

        old_current = next;
    }

    free(new_clusters);
    return FAT16_WRITE_OK;

rollback:
    /*
     * The directory has not been committed, so the old
     * file remains the active version. Release the new
     * clusters before returning the failure.
     */
    for (u32 i = 0; i < allocated_count; ++i) {
        fat16_clear_cluster(
            fat_lba,
            sectors_per_fat,
            fat_count,
            new_clusters[i],
            disk
        );
    }

    free(new_clusters);
    return failure_result;
}

int fat16_delete_file(const char name[11]) {
    diagnostics_log("remove: FAT16 delete routine entered.");

    if (!name || !disktable || !volumes ||
        disktable[0].parts[0].fs != fat16)
        return FAT16_WRITE_BAD_FS;

    unsigned int disk = volumes[0].disk;

    if (disk >= 4)
        return FAT16_WRITE_BAD_FS;
    if (fat_detect(disk) != 16)
        return FAT16_WRITE_BAD_FS;

    u8 mbr[512], boot[512], sector[512], verify[512];

    if (!disk_read_sector(0, mbr, disk))
        return FAT16_WRITE_IO_ERROR;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return FAT16_WRITE_BAD_FS;

    u32 partition_lba =
        (u32)mbr[454] |
        ((u32)mbr[455] << 8) |
        ((u32)mbr[456] << 16) |
        ((u32)mbr[457] << 24);

    if (!disk_read_sector(partition_lba, boot, disk))
        return FAT16_WRITE_IO_ERROR;

    u16 bytes_per_sector =
        (u16)boot[11] | ((u16)boot[12] << 8);
    u8 sectors_per_cluster = boot[13];
    u16 reserved_sectors =
        (u16)boot[14] | ((u16)boot[15] << 8);
    u8 fat_count = boot[16];
    u16 root_entries =
        (u16)boot[17] | ((u16)boot[18] << 8);
    u16 sectors_per_fat =
        (u16)boot[22] | ((u16)boot[23] << 8);

    u32 total_sectors =
        (u32)boot[19] | ((u32)boot[20] << 8);

    if (total_sectors == 0) {
        total_sectors =
            (u32)boot[32] |
            ((u32)boot[33] << 8) |
            ((u32)boot[34] << 16) |
            ((u32)boot[35] << 24);
    }
    u64 partition_end = (u64)partition_lba + total_sectors;
    if (partition_lba == 0 || total_sectors == 0 || partition_end > 0x100000000ULL || (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
        return FAT16_WRITE_BAD_FS;

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0 ||
        total_sectors == 0)
        return FAT16_WRITE_BAD_FS;

    u32 root_sectors =
        ((u32)root_entries * 32 + 511) / 512;
    u32 fat_lba =
        partition_lba + reserved_sectors;
    u32 root_lba =
        fat_lba + (u32)fat_count * sectors_per_fat;
    u32 metadata_sectors =
        (u32)reserved_sectors +
        (u32)fat_count * sectors_per_fat +
        root_sectors;

    if (total_sectors <= metadata_sectors)
        return FAT16_WRITE_BAD_FS;

    u32 cluster_count =
        (total_sectors - metadata_sectors) /
        sectors_per_cluster;
    u32 max_cluster = cluster_count + 1;

    if (max_cluster > 0xFFEF)
        max_cluster = 0xFFEF;

    u32 fat_entry_count =
        (u32)sectors_per_fat * 512 / 2;

    if (fat_entry_count < 3)
        return FAT16_WRITE_BAD_FS;

    if (max_cluster >= fat_entry_count)
        max_cluster = fat_entry_count - 1;

    if (max_cluster < 2)
        return FAT16_WRITE_BAD_FS;

    diagnostics_log("remove: scanning FAT16 root directory.");

    bool found = false;
    bool end_of_directory = false;
    u32 dir_lba = 0;
    u32 dir_off = 0;
    u32 entries_seen = 0;
    u8 dir_entry[32];

    for (u32 s = 0;
         s < root_sectors && !found && !end_of_directory;
         ++s) {
        if (!disk_read_sector(root_lba + s, sector, disk))
            return FAT16_WRITE_IO_ERROR;

        for (u32 off = 0;
             off < 512 && entries_seen < root_entries;
             off += 32, ++entries_seen) {
            u8 first = sector[off];

            if (first == 0x00) {
                end_of_directory = true;
                break;
            }

            if (first == 0xE5)
                continue;

            u8 attr = sector[off + 11];

            if (attr == 0x0F || (attr & 0x08))
                continue;

            bool same = true;
            for (u32 i = 0; i < 11; ++i) {
                if (sector[off + i] != (u8)name[i]) {
                    same = false;
                    break;
                }
            }

            if (same) {
                memcpy(dir_entry, &sector[off], 32);
                dir_lba = root_lba + s;
                dir_off = off;
                found = true;
                break;
            }
        }
    }

    if (!found)
        return FAT16_WRITE_NOT_FOUND;

    if (dir_entry[11] & 0x10)
        return FAT16_WRITE_NOT_FILE;

    diagnostics_log("remove: file located; validating FAT chain.");

    u16 first_cluster =
        (u16)dir_entry[26] | ((u16)dir_entry[27] << 8);
    u16 high_cluster =
        (u16)dir_entry[20] | ((u16)dir_entry[21] << 8);
    u32 file_size =
        (u32)dir_entry[28] |
        ((u32)dir_entry[29] << 8) |
        ((u32)dir_entry[30] << 16) |
        ((u32)dir_entry[31] << 24);

    if (high_cluster != 0)
        return FAT16_WRITE_BAD_FS;

    u32 bytes_per_cluster =
        (u32)sectors_per_cluster * 512;

    u32 needed = file_size / bytes_per_cluster;
    if (file_size % bytes_per_cluster)
        ++needed;

    if (needed > max_cluster - 1)
        return FAT16_WRITE_BAD_FS;

    if (needed == 0) {
        if (first_cluster != 0)
            return FAT16_WRITE_BAD_FS;
    } else if (first_cluster < 2 || first_cluster > max_cluster) {
        return FAT16_WRITE_BAD_FS;
    }

    /*
     * Pass one: validate every link without allocating a
     * disk-sized array. The exact size-derived length and the
     * final end-of-chain marker reject loops and truncated chains.
     */
    u16 current = first_cluster;

    for (u32 step = 0; step < needed; ++step) {
        if (current < 2 || current > max_cluster)
            return FAT16_WRITE_BAD_FS;

        u32 offset = (u32)current * 2;
        if (offset + 1 >= (u32)sectors_per_fat * 512U)
            return FAT16_WRITE_BAD_FS;

        if (!disk_read_sector(fat_lba + offset / 512, sector, disk))
            return FAT16_WRITE_IO_ERROR;

        u32 pos = offset % 512;
        u16 next =
            (u16)sector[pos] | ((u16)sector[pos + 1] << 8);

        if (step + 1 == needed) {
            if (next < 0xFFF8)
                return FAT16_WRITE_BAD_FS;
        } else {
            if (next < 2 || next > max_cluster || next >= 0xFFF0)
                return FAT16_WRITE_BAD_FS;
            current = next;
        }
    }

    diagnostics_log("remove: FAT chain validated; rechecking directory entry.");

    if (!disk_read_sector(dir_lba, sector, disk))
        return FAT16_WRITE_IO_ERROR;

    bool same_name = true;
    for (u32 i = 0; i < 11; ++i) {
        if (sector[dir_off + i] != (u8)name[i]) {
            same_name = false;
            break;
        }
    }

    u16 current_cluster =
        (u16)sector[dir_off + 26] |
        ((u16)sector[dir_off + 27] << 8);
    u16 current_high =
        (u16)sector[dir_off + 20] |
        ((u16)sector[dir_off + 21] << 8);
    u32 current_size =
        (u32)sector[dir_off + 28] |
        ((u32)sector[dir_off + 29] << 8) |
        ((u32)sector[dir_off + 30] << 16) |
        ((u32)sector[dir_off + 31] << 24);

    if (!same_name ||
        (sector[dir_off + 11] & 0x10) ||
        current_cluster != first_cluster ||
        current_high != high_cluster ||
        current_size != file_size)
        return FAT16_WRITE_NOT_FOUND;

    diagnostics_log("remove: marking directory entry deleted.");
    sector[dir_off] = 0xE5;

    if (!disk_write_sector(dir_lba, sector, disk))
        return FAT16_WRITE_IO_ERROR;

    if (!disk_read_sector(dir_lba, verify, disk))
        return FAT16_WRITE_IO_ERROR;

    if (verify[dir_off] != 0xE5)
        return FAT16_WRITE_IO_ERROR;

    diagnostics_log("remove: directory entry deletion verified.");
    diagnostics_log("remove: releasing file clusters.");

    /*
     * Pass two: read the next link before clearing the current
     * cluster, then release it in each FAT copy.
     */
    current = first_cluster;

    for (u32 step = 0; step < needed; ++step) {
        u16 next = 0xFFFF;

        if (step + 1 < needed) {
            u32 offset = (u32)current * 2;

            if (!disk_read_sector(fat_lba + offset / 512, sector, disk))
                return FAT16_WRITE_DELETE_CLEANUP_ERROR;

            u32 pos = offset % 512;
            next =
                (u16)sector[pos] | ((u16)sector[pos + 1] << 8);

            if (next < 2 || next > max_cluster || next >= 0xFFF0)
                return FAT16_WRITE_DELETE_CLEANUP_ERROR;
        }

        for (u32 copy = 0; copy < fat_count; ++copy) {
            if (!fat16_write_fat_entry_copy(
                    fat_lba + copy * sectors_per_fat,
                    sectors_per_fat,
                    current,
                    0,
                    disk))
                return FAT16_WRITE_DELETE_CLEANUP_ERROR;
        }

        current = next;
    }

    diagnostics_log("remove: deletion completed.");
    return FAT16_WRITE_OK;
}

int fat16_dir_open(DIR *dir, const char *path) {
    if (!dir || !path || !volumes || !disktable || dir->volume >= LETTERS)
        return 0;

    u32 path_length = 0;
    while (path[path_length]) {
        if (path_length >= FILENAME_LENGTH - 1)
            return 0;
        ++path_length;
    }
    if (path_length < 3 || path[0] != '|' || path[1] != (char)('A' + dir->volume) || path[2] != '/')
        return 0;
    unsigned int disk = volumes[dir->volume].disk;
    if (disk >= 4)
        return 0;

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

    u32 total_sectors =
        (u16)boot[19] |
        ((u32)boot[20] << 8);

    if (total_sectors == 0) {
        total_sectors =
            (u32)boot[32] |
            ((u32)boot[33] << 8) |
            ((u32)boot[34] << 16) |
            ((u32)boot[35] << 24);
    }

    u64 root_sectors_wide = ((u64)root_entries * 32 + 511) / 512;
    u64 metadata_sectors = (u64)reserved_sectors + (u64)fat_count * sectors_per_fat + root_sectors_wide;
    u64 partition_end = (u64)partition_lba + total_sectors;

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0 ||
        total_sectors == 0 ||
        metadata_sectors >= total_sectors ||
        partition_end > 0x100000000ULL ||
        (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
        return 0;

    u32 root_sectors = (u32)root_sectors_wide;
    u32 cluster_count = (total_sectors - (u32)metadata_sectors) / sectors_per_cluster;
    if (cluster_count == 0 || cluster_count > 0xFFEE || (u64)cluster_count + 2 > (u64)sectors_per_fat * 256)
        return 0;

    u32 fat_lba = partition_lba + reserved_sectors;
    u32 root_lba = fat_lba + (u32)fat_count * sectors_per_fat;
    u32 data_lba = root_lba + root_sectors;

    fat16_dir_state_t *state =
        (fat16_dir_state_t *)dir->fsdata;

    memset(state, 0, sizeof(fat16_dir_state_t));

    state->root_lba = root_lba;
    state->root_sectors = root_sectors;
    state->fat_lba = fat_lba;
    state->data_lba = data_lba;
    state->sectors_per_cluster = sectors_per_cluster;
    state->cluster_limit = cluster_count;
    state->clusters_seen = 0;

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

if (*cursor)
        return 0;

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
                cluster_count,
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

        if (dir_cluster < 2 || (u32)dir_cluster - 2 >= cluster_count)
            return 0;

        root = false;
    }

    state->root = false;
    state->cluster = dir_cluster;
    state->clusters_seen = 1;
    state->cluster_sector = 0;
    state->entry_index = 0;
    state->done = false;

    return 1;
}

int fat16_dir_read(DIR *dir) {
    if (!dir || !volumes || !disktable || dir->volume >= LETTERS)
        return 0;

    unsigned int disk = volumes[dir->volume].disk;
    if (disk >= 4) {
        dir->reserved = 1;
        return 0;
    }

    fat16_dir_state_t *state =
        (fat16_dir_state_t *)dir->fsdata;

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
                dir->reserved = 1;
                state->done = true;
                diagnostics_log("FAT16 directory: root-sector read failed.");
                return 0;
            }

        } else {

            if (state->cluster < 2 ||
                state->cluster >= 0xFFF0 ||
                state->cluster_limit == 0 ||
                (u32)state->cluster - 2 >= state->cluster_limit) {

                dir->reserved = 1;
                state->done = true;
                diagnostics_log("FAT16 directory: current cluster is outside the volume bounds.");
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
                dir->reserved = 1;
                state->done = true;
                diagnostics_log("FAT16 directory: data-sector read failed.");
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

                u16 next;
                if (!fat16_next_cluster(state->fat_lba, state->cluster, disk, &next)) {
                    dir->reserved = 1;
                    state->done = true;
                    diagnostics_log("FAT16 directory: failed to read the next cluster.");
                    return 0;
                }
                if (next >= 0xFFF8) {
                    state->done = true;
                    return 0;
                }
                if (next < 2 || next >= 0xFFF0 || state->cluster_limit == 0 || (u32)next - 2 >= state->cluster_limit || state->clusters_seen >= state->cluster_limit) {
                    dir->reserved = 1;
                    state->done = true;
                    diagnostics_log("FAT16 directory: cluster chain is corrupt or exceeds its traversal limit.");
                    return 0;
                }
                ++state->clusters_seen;
                state->cluster = next;
                state->cluster_sector = 0;
            }
        }
    }

    return 0;
}

size_t fat16_read(void *ptr, size_t bytes, FILE *fp) {
    if (!ptr || !fp || !fp->path || !volumes || !disktable || fp->volume >= LETTERS)
        return 0;

    if (fp->pointer >= fp->size)
        return 0;

    size_t available = fp->size - fp->pointer;

    if (bytes > available)
        bytes = available;

    if (bytes == 0)
        return 0;

    unsigned int disk = volumes[fp->volume].disk;

    if (disk >= 4)
        return 0;
    unsigned int partition = volumes[fp->volume].partition;
    if (disk >= 4 || partition >= 4 || disktable[disk].parts[partition].fs != fat16)
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

    u32 total_sectors =
        (u32)boot[19] |
        ((u32)boot[20] << 8);

    if (total_sectors == 0) {
        total_sectors =
            (u32)boot[32] |
            ((u32)boot[33] << 8) |
            ((u32)boot[34] << 16) |
            ((u32)boot[35] << 24);
    }

    u64 root_sectors_wide = ((u64)root_entries * 32 + 511) / 512;
    u64 metadata_sectors = (u64)reserved_sectors + (u64)fat_count * sectors_per_fat + root_sectors_wide;
    u64 partition_end = (u64)partition_lba + total_sectors;

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0 ||
        total_sectors == 0 ||
        metadata_sectors >= total_sectors ||
        partition_end > 0x100000000ULL ||
        (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
        return 0;

    u32 root_sectors = (u32)root_sectors_wide;
    u32 cluster_count = (total_sectors - (u32)metadata_sectors) / sectors_per_cluster;
    if (cluster_count == 0 || cluster_count > 0xFFEE || (u64)cluster_count + 2 > (u64)sectors_per_fat * 256)
        return 0;

    u32 fat_lba =
        partition_lba + reserved_sectors;

    u32 root_lba =
        fat_lba + ((u32)fat_count * sectors_per_fat);

    u32 data_lba =
        root_lba + root_sectors;

    FAT_entry entry;

    if (!fat16_find_file_entry(fp, &entry))
        return 0;
    if ((unsigned long long)entry.size != fp->size || entry.cluster_high != 0) {
        diagnostics_log("FAT16 read: opened file metadata no longer matches the directory entry.");
        return 0;
    }
    u16 cluster =
        (u16)entry.cluster_low;

    if (cluster < 2 || cluster >= 0xFFF0 || (u32)cluster - 2 >= cluster_count)
        return 0;

    u32 bytes_per_cluster =
        (u32)bytes_per_sector * sectors_per_cluster;

    u32 skipped_clusters =
        fp->pointer / bytes_per_cluster;

    u32 offset =
        fp->pointer % bytes_per_cluster;

    for (u32 i = 0; i < skipped_clusters; ++i) {
        u16 next;
        if (!fat16_next_cluster(fat_lba, cluster, disk, &next)) {
            diagnostics_log("FAT16 read: failed to read the next cluster.");
            return 0;
        }
        if (next < 2 || next >= 0xFFF0 || (u32)next - 2 >= cluster_count) {
            diagnostics_log("FAT16 read: cluster chain is invalid before the requested offset.");
            return 0;
        }
        cluster = next;
    }

    u8 sector[512];
    size_t remaining = bytes;
    size_t copied = 0;

    while (remaining > 0) {
        if (cluster < 2 || cluster >= 0xFFF0 || (u32)cluster - 2 >= cluster_count) {
            diagnostics_log("FAT16 read: cluster chain is outside the valid data region.");
            fp->pointer += copied;
            return copied;
        }
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
                    disk)) {
                fp->pointer += copied;
                return copied;
            }

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

        u16 next;
        if (!fat16_next_cluster(fat_lba, cluster, disk, &next)) {
            diagnostics_log("FAT16 read: failed to read the next cluster.");
            fp->pointer += copied;
            return copied;
        }
        if (next < 2 || next >= 0xFFF0 || (u32)next - 2 >= cluster_count) {
            diagnostics_log("FAT16 read: cluster chain is invalid before the file contents were fully read.");
            fp->pointer += copied;
            return copied;
        }
        cluster = next;
        offset = 0;
    }

    fp->pointer += copied;

    return copied;
}

int fat_cat(unsigned int disk, const char *filename) {
    u8 mbr[512];
    u8 boot[512];
    u8 sector[512];

    if (!filename || !*filename || !disktable || disk >= 4)
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

    u32 total_sectors =
        (u16)boot[19] |
        ((u32)boot[20] << 8);

    if (total_sectors == 0) {
        total_sectors =
            (u32)boot[32] |
            ((u32)boot[33] << 8) |
            ((u32)boot[34] << 16) |
            ((u32)boot[35] << 24);
    }

    u64 root_sectors_wide = ((u64)root_entries * 32 + 511) / 512;
    u64 metadata_sectors = (u64)reserved_sectors + (u64)fat_count * sectors_per_fat + root_sectors_wide;
    u64 partition_end = (u64)partition_lba + total_sectors;

    if (bytes_per_sector != 512 ||
        sectors_per_cluster == 0 ||
        sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 ||
        fat_count == 0 ||
        root_entries == 0 ||
        sectors_per_fat == 0 ||
        total_sectors == 0 ||
        metadata_sectors >= total_sectors ||
        partition_end > 0x100000000ULL ||
        (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks))
        return 0;

    u32 root_sectors = (u32)root_sectors_wide;
    u32 cluster_count = (total_sectors - (u32)metadata_sectors) / sectors_per_cluster;
    if (cluster_count == 0 || cluster_count > 0xFFEE || (u64)cluster_count + 2 > (u64)sectors_per_fat * 256)
        return 0;

    u32 fat_lba = partition_lba + reserved_sectors;
    u32 root_lba = fat_lba + (u32)fat_count * sectors_per_fat;
    u32 data_lba = root_lba + root_sectors;

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
                cluster_count,
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

        u16 next;
        if (!fat16_next_cluster(fat_lba, cluster, disk, &next)) {
            diagnostics_log("FAT16 file display: cluster-chain read failed.");
            return 0;
        }
        if (next < 2 || next == 0xFFF7 || next >= 0xFFF8) {
            diagnostics_log("FAT16 file display: cluster chain ended before the full file was read.");
            return 0;
        }
        cluster = next;
    }

    terminal_write("\n");

    return remaining == 0;
}
