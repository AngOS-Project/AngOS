#include <types.h>
#include <string.h>
#include <disk.h>
#include "FATWrite.h"

#define FAT_WRITE_ENTRY_SIZE 32
#define FAT_WRITE_ENTRIES_PER_SECTOR 16

static u16 fat_write_u16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static void fat_write_put16(u8 *p, u16 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
}

static void fat_write_put32(u8 *p, u32 value) {
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static char fat_write_upper(char c) {
    if (c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

static bool fat_write_valid_char(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    switch (c) {
        case '$': case '%': case '\'': case '-': case '_': case '@':
        case '~': case '`': case '!': case '(': case ')': case '{':
        case '}': case '^': case '#': case '&':
            return true;
        default:
            return false;
    }
}

static bool fat_write_encode_name(const char *input, u8 output[11]) {
    if (!input || !output || !input[0])
        return false;
    for (u32 i = 0; i < 11; ++i)
        output[i] = ' ';
    u32 base = 0;
    u32 extension = 0;
    bool after_dot = false;
    for (u32 i = 0; input[i]; ++i) {
        char c = fat_write_upper(input[i]);
        if (c == '.') {
            if (after_dot || base == 0)
                return false;
            after_dot = true;
            continue;
        }
        if (!fat_write_valid_char(c))
            return false;
        if (!after_dot) {
            if (base >= 8)
                return false;
            output[base++] = (u8)c;
        } else {
            if (extension >= 3)
                return false;
            output[8 + extension++] = (u8)c;
        }
    }
    return base != 0 && (!after_dot || extension != 0);
}

typedef struct {
    u64 lba;
    u64 next_lba;
    u16 offset;
    u16 next_offset;
    u8 found;
    u8 existing;
    u8 was_end;
    u8 next_valid;
    u8 raw[FAT_WRITE_ENTRY_SIZE];
} fat_write_slot;

static void fat_write_save_slot(
    fat_write_slot *slot,
    u64 lba,
    u16 offset,
    const u8 *raw,
    bool existing,
    bool was_end,
    u64 next_lba,
    u16 next_offset,
    bool next_valid
) {
    slot->lba = lba;
    slot->offset = offset;
    slot->found = 1;
    slot->existing = existing ? 1 : 0;
    slot->was_end = was_end ? 1 : 0;
    slot->next_lba = next_lba;
    slot->next_offset = next_offset;
    slot->next_valid = next_valid ? 1 : 0;
    memcpy(slot->raw, raw, FAT_WRITE_ENTRY_SIZE);
}

static int fat_write_scan_sector(
    u64 lba,
    const u8 name[11],
    fat_write_slot *slot,
    bool *free_found,
    bool *directory_end,
    unsigned int disk,
    u32 entry_limit,
    u64 boundary_next_lba,
    bool boundary_next_valid
) {
    u8 sector[SECTOR_SIZE];
    if (!disk_read_sector(lba, sector, disk))
        return -1;
    if (entry_limit > FAT_WRITE_ENTRIES_PER_SECTOR)
        entry_limit = FAT_WRITE_ENTRIES_PER_SECTOR;
    for (u32 index = 0; index < entry_limit; ++index) {
        u16 offset = (u16)(index * FAT_WRITE_ENTRY_SIZE);
        const u8 *entry = &sector[offset];
        if (entry[0] == 0x00) {
            if (!*free_found) {
                u64 next_lba = boundary_next_lba;
                u16 next_offset = 0;
                bool next_valid = boundary_next_valid;
                if (index + 1U < entry_limit) {
                    next_lba = lba;
                    next_offset = (u16)(offset + FAT_WRITE_ENTRY_SIZE);
                    next_valid = true;
                }
                fat_write_save_slot(slot, lba, offset, entry, false, true, next_lba, next_offset, next_valid);
                *free_found = true;
            }
            *directory_end = true;
            return 0;
        }
        if (entry[0] == 0xE5) {
            if (!*free_found) {
                fat_write_save_slot(slot, lba, offset, entry, false, false, 0, 0, false);
                *free_found = true;
            }
            continue;
        }
        if (entry[11] == 0x0F || (entry[11] & 0x08U))
            continue;
        if (memcmp(entry, name, 11) == 0) {
            if (entry[11] & 0x10U)
                return -2;
            fat_write_save_slot(slot, lba, offset, entry, true, false, 0, 0, false);
            return 1;
        }
    }
    return 0;
}

static bool fat_write_extend_directory(fat_node_fs *filesystem, const fat_node *directory, u64 *lba) {
    if (!filesystem || !directory || !lba)
        return false;
    const fat_native_volume *volume = &filesystem->volume;
    if (directory->fixed_root ||
        directory->first_cluster < 2 || directory->first_cluster > volume->cluster_count + 1)
        return false;
    u32 current = directory->first_cluster;
    u32 last = current;
    bool ended = false;
    for (u32 step = 0; step < volume->cluster_count; ++step) {
        u32 next = 0;
        int state = fat_native_chain_next(volume, current, &next);
        if (state == FAT_NATIVE_CHAIN_ERROR)
            return false;
        if (state == FAT_NATIVE_CHAIN_END) {
            last = current;
            ended = true;
            break;
        }
        current = next;
    }
    if (!ended)
        return false;
    u32 added = 0;
    if (!fat_native_allocate_chain(&filesystem->volume, 1, &added) || added < 2)
        return false;
    u8 zero[SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));
    for (u8 sector = 0; sector < volume->sectors_per_cluster; ++sector) {
        u64 target;
        if (!fat_native_cluster_sector(volume, added, sector, &target) ||
            !disk_write_sector(target, zero, volume->disk)) {
            fat_native_release_chain(volume, added);
            return false;
        }
    }
    if (!fat_native_cluster_sector(volume, added, 0, lba)) {
        fat_native_release_chain(volume, added);
        return false;
    }
    if (!fat_native_fat_write(volume, last, added)) {
        u32 eoc = volume->fat_bits == FAT_NATIVE_FAT12 ? 0x0FFFU : volume->fat_bits == FAT_NATIVE_FAT16 ? 0xFFFFU : 0x0FFFFFFFU;
        fat_native_fat_write(volume, last, eoc);
        fat_native_release_chain(volume, added);
        return false;
    }
    return true;
}

static int fat_write_find_slot(
    fat_node_fs *filesystem,
    const fat_node *directory,
    const u8 name[11],
    fat_write_slot *slot
) {
    if (!filesystem || !directory || !name || !slot || !(directory->attributes & 0x10U))
        return FAT_WRITE_INVALID;
    memset(slot, 0, sizeof(*slot));
    bool free_found = false;
    bool directory_end = false;
    const fat_native_volume *volume = &filesystem->volume;
    if (directory->fixed_root) {
        for (u32 sector = 0; sector < volume->root_sectors; ++sector) {
            u64 lba = volume->partition_lba + volume->root_start + sector;
            u32 remaining_entries = filesystem->root_entries - sector * FAT_WRITE_ENTRIES_PER_SECTOR;
            u32 entry_limit = remaining_entries > FAT_WRITE_ENTRIES_PER_SECTOR ? FAT_WRITE_ENTRIES_PER_SECTOR : remaining_entries;
            u64 boundary_next_lba = lba + 1U;
            bool boundary_next_valid = sector + 1U < volume->root_sectors;
            int result = fat_write_scan_sector(lba, name, slot, &free_found, &directory_end, volume->disk, entry_limit, boundary_next_lba, boundary_next_valid);
            if (result == 1)
                return 1;
            if (result < 0)
                return result == -2 ? FAT_WRITE_INVALID : FAT_WRITE_IO_ERROR;
            if (directory_end)
                return FAT_WRITE_NO_SPACE;
        }
        return FAT_WRITE_NO_SPACE;
    }
    if (directory->first_cluster < 2 || directory->first_cluster > volume->cluster_count + 1)
        return FAT_WRITE_INVALID;
    u32 current = directory->first_cluster;
    for (u32 step = 0; step < volume->cluster_count; ++step) {
        u32 next = 0;
        int state = fat_native_chain_next(volume, current, &next);
        if (state == FAT_NATIVE_CHAIN_ERROR)
            return FAT_WRITE_IO_ERROR;
        for (u8 sector = 0; sector < volume->sectors_per_cluster; ++sector) {
            u64 lba;
            if (!fat_native_cluster_sector(volume, current, sector, &lba))
                return FAT_WRITE_IO_ERROR;
            u64 boundary_next_lba = 0;
            bool boundary_next_valid = false;
            if (sector + 1U < volume->sectors_per_cluster) {
                boundary_next_lba = lba + 1U;
                boundary_next_valid = true;
            } else if (state == FAT_NATIVE_CHAIN_NEXT) {
                if (!fat_native_cluster_sector(volume, next, 0, &boundary_next_lba))
                    return FAT_WRITE_IO_ERROR;
                boundary_next_valid = true;
            }
            int result = fat_write_scan_sector(lba, name, slot, &free_found, &directory_end, volume->disk, FAT_WRITE_ENTRIES_PER_SECTOR, boundary_next_lba, boundary_next_valid);
            if (result == 1)
                return 1;
            if (result < 0)
                return result == -2 ? FAT_WRITE_INVALID : FAT_WRITE_IO_ERROR;
            if (directory_end)
                return FAT_WRITE_NO_SPACE;
        }
        if (state == FAT_NATIVE_CHAIN_END) {
            if (free_found)
                return FAT_WRITE_NO_SPACE;
            u64 new_lba;
            if (!fat_write_extend_directory(filesystem, directory, &new_lba))
                return FAT_WRITE_NO_SPACE;
            u8 empty[FAT_WRITE_ENTRY_SIZE];
            memset(empty, 0, sizeof(empty));
            fat_write_save_slot(slot, new_lba, 0, empty, false, true, new_lba, FAT_WRITE_ENTRY_SIZE, true);
            return FAT_WRITE_NO_SPACE;
        }
        current = next;
    }
    return FAT_WRITE_IO_ERROR;
}

static bool fat_write_data_chain(
    const fat_native_volume *volume,
    u32 first_cluster,
    const u8 *data,
    u32 length
) {
    if (length == 0)
        return first_cluster == 0;
    if (!volume || !data || first_cluster < 2)
        return false;
    u32 cluster_bytes = (u32)volume->bytes_per_sector * volume->sectors_per_cluster;
    if (cluster_bytes == 0)
        return false;
    u32 current = first_cluster;
    u32 position = 0;
    u64 needed = ((u64)length + cluster_bytes - 1U) / cluster_bytes;
    for (u64 cluster_index = 0; cluster_index < needed; ++cluster_index) {
        for (u8 sector_index = 0; sector_index < volume->sectors_per_cluster; ++sector_index) {
            u8 sector[SECTOR_SIZE];
            memset(sector, 0, sizeof(sector));
            u32 remaining = length - position;
            u32 amount = remaining > volume->bytes_per_sector ? volume->bytes_per_sector : remaining;
            if (amount)
                memcpy(sector, data + position, amount);
            u64 lba;
            if (!fat_native_cluster_sector(volume, current, sector_index, &lba) ||
                !disk_write_sector(lba, sector, volume->disk))
                return false;
            position += amount;
        }
        if (cluster_index + 1 < needed) {
            u32 next = 0;
            if (fat_native_chain_next(volume, current, &next) != FAT_NATIVE_CHAIN_NEXT)
                return false;
            current = next;
        }
    }
    return position == length;
}

static int fat_write_commit_entry(const fat_node_fs *filesystem, fat_write_slot *slot, const u8 name[11], u32 first_cluster, u32 length) {
    if (!filesystem || !slot || !slot->found)
        return 0;
    u8 sector[SECTOR_SIZE];
    if (!disk_read_sector(slot->lba, sector, filesystem->volume.disk))
        return 0;
    if (!slot->existing && slot->was_end && slot->next_valid && slot->next_lba != slot->lba) {
        u8 next_sector[SECTOR_SIZE];
        if (!disk_read_sector(slot->next_lba, next_sector, filesystem->volume.disk))
            return 0;
        if (slot->next_offset > SECTOR_SIZE - FAT_WRITE_ENTRY_SIZE)
            return 0;
        memset(&next_sector[slot->next_offset], 0, FAT_WRITE_ENTRY_SIZE);
        if (!disk_write_sector(slot->next_lba, next_sector, filesystem->volume.disk))
            return 0;
    }
    u8 *entry = &sector[slot->offset];
    if (slot->existing) {
        if (memcmp(entry, slot->raw, FAT_WRITE_ENTRY_SIZE) != 0)
            return 0;
        if (entry[11] & 0x01U)
            return 0;
    } else {
        if (entry[0] != 0x00 && entry[0] != 0xE5)
            return 0;
        memset(entry, 0, FAT_WRITE_ENTRY_SIZE);
        memcpy(entry, name, 11);
        entry[11] = 0x20;
    }
    if (entry[11] & 0x10U)
        return 0;
    if (!slot->existing && slot->was_end && slot->next_valid && slot->next_lba == slot->lba) {
        if (slot->next_offset > SECTOR_SIZE - FAT_WRITE_ENTRY_SIZE)
            return 0;
        memset(&sector[slot->next_offset], 0, FAT_WRITE_ENTRY_SIZE);
    }
    if (filesystem->volume.fat_bits == FAT_NATIVE_FAT32)
        fat_write_put16(&entry[20], (u16)(first_cluster >> 16));
    else
        fat_write_put16(&entry[20], 0);
    fat_write_put16(&entry[26], (u16)first_cluster);
    fat_write_put32(&entry[28], length);
    if (!disk_write_sector(slot->lba, sector, filesystem->volume.disk))
        return -1;
    u8 verify[SECTOR_SIZE];
    if (!disk_read_sector(slot->lba, verify, filesystem->volume.disk))
        return -1;
    return memcmp(&verify[slot->offset], entry, FAT_WRITE_ENTRY_SIZE) == 0 ? 1 : -1;
}

int fat_node_write_file(fat_node_fs *filesystem, const char *path, const void *data, u32 length) {
    if (!filesystem || !path || (!data && length != 0))
        return FAT_WRITE_INVALID;
    u32 path_length = 0;
    while (path[path_length]) {
        if (path_length >= FAT_NODE_NAME_MAX)
            return FAT_WRITE_INVALID;
        ++path_length;
    }
    if (path_length == 0)
        return FAT_WRITE_INVALID;
    char parent_path[FAT_NODE_NAME_MAX];
    char file_name[FAT_NODE_NAME_MAX];
    u32 split = path_length;
    while (split > 0 && path[split - 1] != '/' && path[split - 1] != '\\')
        --split;
    u32 name_length = path_length - split;
    if (name_length == 0 || name_length >= sizeof(file_name))
        return FAT_WRITE_INVALID;
    memcpy(file_name, path + split, name_length);
    file_name[name_length] = '\0';
    u8 short_name[11];
    if (!fat_write_encode_name(file_name, short_name))
        return FAT_WRITE_INVALID;
    fat_node directory;
    if (split == 0) {
        if (!fat_node_root(filesystem, &directory))
            return FAT_WRITE_INVALID;
    } else {
        u32 parent_length = split - 1;
        if (parent_length >= sizeof(parent_path))
            return FAT_WRITE_INVALID;
        memcpy(parent_path, path, parent_length);
        parent_path[parent_length] = '\0';
        if (parent_length == 0) {
            if (!fat_node_root(filesystem, &directory))
                return FAT_WRITE_INVALID;
        } else if (!fat_node_lookup(filesystem, parent_path, &directory)) {
            return FAT_WRITE_INVALID;
        }
    }
    if (!(directory.attributes & 0x10U))
        return FAT_WRITE_INVALID;
    fat_write_slot slot;
    int slot_state = fat_write_find_slot(filesystem, &directory, short_name, &slot);
    if (slot_state == FAT_WRITE_INVALID || slot_state == FAT_WRITE_IO_ERROR)
        return slot_state;
    if (slot_state == FAT_WRITE_NO_SPACE && !slot.found)
        return FAT_WRITE_NO_SPACE;
    if (slot.existing && (slot.raw[11] & 0x01U))
        return FAT_WRITE_READ_ONLY;
    u32 old_cluster = slot.existing ? ((u32)fat_write_u16(&slot.raw[20]) << 16) | fat_write_u16(&slot.raw[26]) : 0;
    if (filesystem->volume.fat_bits == FAT_NATIVE_FAT32)
        old_cluster &= 0x0FFFFFFFU;
    else
        old_cluster &= 0xFFFFU;
    u32 cluster_bytes = (u32)filesystem->volume.bytes_per_sector * filesystem->volume.sectors_per_cluster;
    if (cluster_bytes == 0)
        return FAT_WRITE_INVALID;
    u32 cluster_count = length == 0 ? 0 : (u32)(((u64)length + cluster_bytes - 1U) / cluster_bytes);
    u32 new_cluster = 0;
    if (cluster_count && !fat_native_allocate_chain(&filesystem->volume, cluster_count, &new_cluster))
        return FAT_WRITE_NO_SPACE;
    if (cluster_count && !fat_write_data_chain(&filesystem->volume, new_cluster, data, length)) {
        fat_native_release_chain(&filesystem->volume, new_cluster);
        return FAT_WRITE_IO_ERROR;
    }
    int committed = fat_write_commit_entry(filesystem, &slot, short_name, new_cluster, length);
    if (committed != 1) {
        if (new_cluster && committed == 0)
            fat_native_release_chain(&filesystem->volume, new_cluster);
        return FAT_WRITE_IO_ERROR;
    }
    if (old_cluster && old_cluster != new_cluster && !fat_native_release_chain(&filesystem->volume, old_cluster))
        return FAT_WRITE_CLEANUP_ERROR;
    return FAT_WRITE_OK;
}
