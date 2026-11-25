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


static void fat_write_init_directory_entry(
    u8 entry[FAT_WRITE_ENTRY_SIZE],
    const u8 name[11],
    u32 cluster,
    const fat_native_volume *volume
) {
    memset(entry, 0, FAT_WRITE_ENTRY_SIZE);
    memcpy(entry, name, 11);
    entry[11] = 0x10U;
    if (volume->fat_bits == FAT_NATIVE_FAT32)
        fat_write_put16(&entry[20], (u16)(cluster >> 16));
    fat_write_put16(&entry[26], (u16)cluster);
}

static int fat_write_commit_directory_entry(
    const fat_node_fs *filesystem,
    fat_write_slot *slot,
    const u8 name[11],
    u32 cluster
) {
    if (!filesystem || !slot || !slot->found || slot->existing ||
        slot->offset > SECTOR_SIZE - FAT_WRITE_ENTRY_SIZE)
        return 0;

    u8 sector[SECTOR_SIZE];
    if (!disk_read_sector(slot->lba, sector, filesystem->volume.disk))
        return 0;

    if (slot->was_end && slot->next_valid && slot->next_lba != slot->lba) {
        u8 next_sector[SECTOR_SIZE];
        if (slot->next_offset > SECTOR_SIZE - FAT_WRITE_ENTRY_SIZE ||
            !disk_read_sector(slot->next_lba, next_sector, filesystem->volume.disk))
            return 0;
        memset(&next_sector[slot->next_offset], 0, FAT_WRITE_ENTRY_SIZE);
        if (!disk_write_sector(slot->next_lba, next_sector, filesystem->volume.disk))
            return -1;
    }

    u8 *entry = &sector[slot->offset];
    if ((entry[0] != 0x00U && entry[0] != 0xE5U) ||
        memcmp(entry, slot->raw, FAT_WRITE_ENTRY_SIZE) != 0)
        return 0;

    fat_write_init_directory_entry(entry, name, cluster, &filesystem->volume);

    if (slot->was_end && slot->next_valid && slot->next_lba == slot->lba) {
        if (slot->next_offset > SECTOR_SIZE - FAT_WRITE_ENTRY_SIZE)
            return 0;
        memset(&sector[slot->next_offset], 0, FAT_WRITE_ENTRY_SIZE);
    }

    if (!disk_write_sector(slot->lba, sector, filesystem->volume.disk))
        return -1;

    u8 verify[SECTOR_SIZE];
    if (!disk_read_sector(slot->lba, verify, filesystem->volume.disk))
        return -1;

    return memcmp(&verify[slot->offset], entry, FAT_WRITE_ENTRY_SIZE) == 0 ? 1 : -1;
}

#define FAT_WRITE_DELETE_MAX_SECTORS 3

typedef struct {
    u64 lba;
    u8 original[SECTOR_SIZE];
    u8 updated[SECTOR_SIZE];
    u8 attempted;
} fat_write_delete_sector;

static bool fat_write_delete_name_equal(const char *a, const char *b) {
    u32 i = 0;
    while (a[i] && b[i]) {
        if (fat_write_upper(a[i]) != fat_write_upper(b[i]))
            return false;
        ++i;
    }
    return a[i] == b[i];
}

static int fat_write_delete_sector_get(
    fat_node_fs *filesystem,
    fat_write_delete_sector sectors[FAT_WRITE_DELETE_MAX_SECTORS],
    u8 *sector_count,
    u64 lba
) {
    for (u8 i = 0; i < *sector_count; ++i) {
        if (sectors[i].lba == lba)
            return i;
    }

    if (*sector_count >= FAT_WRITE_DELETE_MAX_SECTORS)
        return -1;

    u8 index = *sector_count;
    fat_write_delete_sector *sector = &sectors[index];
    if (!disk_read_sector(lba, sector->original, filesystem->volume.disk))
        return -1;

    sector->lba = lba;
    memcpy(sector->updated, sector->original, SECTOR_SIZE);
    sector->attempted = 0;
    ++*sector_count;
    return index;
}

static bool fat_write_delete_mark_slot(
    fat_node_fs *filesystem,
    fat_write_delete_sector sectors[FAT_WRITE_DELETE_MAX_SECTORS],
    u8 *sector_count,
    u64 lba,
    u16 offset,
    const u8 expected[32],
    bool long_name,
    bool directory_entry
) {
    if (offset > SECTOR_SIZE - FAT_WRITE_ENTRY_SIZE || (offset & 31U) != 0)
        return false;

    int index = fat_write_delete_sector_get(
        filesystem, sectors, sector_count, lba
    );
    if (index < 0)
        return false;

    fat_write_delete_sector *sector = &sectors[index];
    const u8 *current = &sector->original[offset];
    if (current[0] == 0x00 || current[0] == 0xE5 ||
        memcmp(current, expected, FAT_WRITE_ENTRY_SIZE) != 0)
        return false;

    if (long_name) {
        if (current[11] != 0x0F || current[0] != expected[0] ||
            current[13] != expected[13])
            return false;
    } else if (((current[11] & 0x10U) != 0) != directory_entry) {
        return false;
    }

    sector->updated[offset] = 0xE5;
    return true;
}

static bool fat_write_delete_chain_valid(
    const fat_native_volume *volume,
    const fat_node *node
) {
    if (!volume || !node)
        return false;

    if (node->size == 0)
        return node->first_cluster == 0;

    if (node->first_cluster < 2 ||
        node->first_cluster > volume->cluster_count + 1)
        return false;

    u32 current = node->first_cluster;
    for (u32 step = 0; step < volume->cluster_count; ++step) {
        u32 next = 0;
        int state = fat_native_chain_next(volume, current, &next);
        if (state == FAT_NATIVE_CHAIN_END)
            return true;
        if (state != FAT_NATIVE_CHAIN_NEXT)
            return false;
        current = next;
    }

    return false;
}


static bool fat_write_delete_directory_chain_valid(
    const fat_native_volume *volume,
    const fat_node *node
) {
    if (!volume || !node ||
        node->first_cluster < 2 ||
        node->first_cluster > volume->cluster_count + 1)
        return false;

    u32 current = node->first_cluster;

    for (u32 step = 0; step < volume->cluster_count; ++step) {
        u32 next = 0;
        int state = fat_native_chain_next(volume, current, &next);

        if (state == FAT_NATIVE_CHAIN_END)
            return true;

        if (state != FAT_NATIVE_CHAIN_NEXT)
            return false;

        current = next;
    }

    return false;
}

static bool fat_write_delete_dot_entry(const char *name) {
    return name &&
        name[0] == '.' &&
        (name[1] == '\0' ||
         (name[1] == '.' && name[2] == '\0'));
}

static void fat_write_delete_rollback(
    fat_node_fs *filesystem,
    fat_write_delete_sector sectors[FAT_WRITE_DELETE_MAX_SECTORS],
    u8 sector_count
) {
    for (u32 i = sector_count; i > 0; --i) {
        fat_write_delete_sector *sector = &sectors[i - 1U];
        if (sector->attempted)
            disk_write_sector(
                sector->lba,
                sector->original,
                filesystem->volume.disk
            );
    }
}

static bool fat_write_delete_write_sector(
    fat_node_fs *filesystem,
    fat_write_delete_sector *sector
) {
    u8 verify[SECTOR_SIZE];
    sector->attempted = 1;

    if (!disk_write_sector(
            sector->lba,
            sector->updated,
            filesystem->volume.disk))
        return false;

    return disk_read_sector(
            sector->lba,
            verify,
            filesystem->volume.disk) &&
        memcmp(verify, sector->updated, SECTOR_SIZE) == 0;
}

static int fat_write_delete_commit(
    fat_node_fs *filesystem,
    const fat_node_entry *entry,
    bool directory_entry
) {
    fat_write_delete_sector sectors[FAT_WRITE_DELETE_MAX_SECTORS];
    memset(sectors, 0, sizeof(sectors));
    u8 sector_count = 0;

    if (entry->lfn_count > FAT_NODE_LFN_MAX)
        return FAT_DELETE_IO_ERROR;

    for (u32 i = 0; i < entry->lfn_count; ++i) {
        const fat_node_dir_slot *slot = &entry->lfn_slots[i];
        if (!fat_write_delete_mark_slot(
                filesystem,
                sectors,
                &sector_count,
                slot->lba,
                slot->offset,
                slot->raw,
                true,
                directory_entry))
            return FAT_DELETE_IO_ERROR;
    }

    int short_index = fat_write_delete_sector_get(
        filesystem, sectors, &sector_count, entry->short_lba
    );
    if (short_index < 0 ||
        !fat_write_delete_mark_slot(
            filesystem,
            sectors,
            &sector_count,
            entry->short_lba,
            entry->short_offset,
            entry->raw,
            false,
            directory_entry))
        return FAT_DELETE_IO_ERROR;

    for (u32 i = 0; i < sector_count; ++i) {
        if ((int)i == short_index)
            continue;
        if (!fat_write_delete_write_sector(filesystem, &sectors[i])) {
            fat_write_delete_rollback(filesystem, sectors, sector_count);
            return FAT_DELETE_IO_ERROR;
        }
    }

    if (!fat_write_delete_write_sector(filesystem, &sectors[short_index])) {
        fat_write_delete_rollback(filesystem, sectors, sector_count);
        return FAT_DELETE_IO_ERROR;
    }

    if (entry->node.first_cluster != 0 &&
        !fat_native_release_chain(
            &filesystem->volume,
            entry->node.first_cluster))
        return FAT_DELETE_CLEANUP_ERROR;

    return FAT_DELETE_OK;
}

int fat_node_delete_file(fat_node_fs *filesystem, const char *path) {
    if (!filesystem || !path || !path[0])
        return FAT_DELETE_INVALID;

    u32 path_length = 0;
    while (path[path_length]) {
        if (path_length >= FAT_NODE_NAME_MAX)
            return FAT_DELETE_INVALID;
        ++path_length;
    }

    u32 split = path_length;
    while (split > 0 && path[split - 1] != '/' && path[split - 1] != '\\')
        --split;

    u32 name_length = path_length - split;
    if (name_length == 0 || name_length > FAT_NODE_NAME_MAX)
        return FAT_DELETE_INVALID;

    char file_name[FAT_NODE_NAME_MAX + 1];
    memcpy(file_name, path + split, name_length);
    file_name[name_length] = '\0';

    char parent_path[FAT_NODE_NAME_MAX + 1];
    u32 parent_length = split == 0 ? 0 : split - 1U;
    if (parent_length > FAT_NODE_NAME_MAX)
        return FAT_DELETE_INVALID;

    memcpy(parent_path, path, parent_length);
    parent_path[parent_length] = '\0';

    fat_node parent;
    if (parent_length == 0) {
        if (!fat_node_root(filesystem, &parent))
            return FAT_DELETE_INVALID;
    } else if (!fat_node_lookup(filesystem, parent_path, &parent)) {
        return FAT_DELETE_NOT_FOUND;
    }

    if (!(parent.attributes & 0x10U))
        return FAT_DELETE_NOT_FOUND;

    fat_node_dir directory;
    if (!fat_node_dir_open(filesystem, &parent, &directory))
        return FAT_DELETE_INVALID;

    fat_node_entry found;
    bool matched = false;
    int state;

    while ((state = fat_node_dir_next(&directory, &found)) == FAT_NODE_ENTRY) {
        if (fat_write_delete_name_equal(file_name, found.name)) {
            matched = true;
            break;
        }
    }

    if (!matched)
        return state == FAT_NODE_ERROR
            ? FAT_DELETE_IO_ERROR
            : FAT_DELETE_NOT_FOUND;

    if (found.type != FAT_NODE_FILE || (found.node.attributes & 0x10U))
        return FAT_DELETE_NOT_FILE;

    if (found.node.attributes & 0x01U)
        return FAT_DELETE_READ_ONLY;

    if (found.lfn_count > FAT_NODE_LFN_MAX ||
        !fat_write_delete_chain_valid(&filesystem->volume, &found.node))
        return FAT_DELETE_IO_ERROR;

    return fat_write_delete_commit(filesystem, &found, false);
}


int fat_node_remove_directory(fat_node_fs *filesystem, const char *path) {
    if (!filesystem || !path || !path[0])
        return FAT_REMOVEDIRECTORY_INVALID;

    u32 path_length = 0;
    while (path[path_length]) {
        if (path_length >= FAT_NODE_NAME_MAX)
            return FAT_REMOVEDIRECTORY_INVALID;
        ++path_length;
    }

    u32 split = path_length;
    while (split > 0 && path[split - 1] != '/' && path[split - 1] != '\\')
        --split;

    u32 name_length = path_length - split;
    if (name_length == 0 || name_length > FAT_NODE_NAME_MAX)
        return FAT_REMOVEDIRECTORY_INVALID;

    char directory_name[FAT_NODE_NAME_MAX + 1];
    memcpy(directory_name, path + split, name_length);
    directory_name[name_length] = '\0';

    char parent_path[FAT_NODE_NAME_MAX + 1];
    fat_node parent;
    u32 parent_length = split == 0 ? 0 : split - 1U;

    if (parent_length == 0) {
        if (!fat_node_root(filesystem, &parent))
            return FAT_REMOVEDIRECTORY_INVALID;
    } else {
        if (parent_length > FAT_NODE_NAME_MAX)
            return FAT_REMOVEDIRECTORY_INVALID;

        memcpy(parent_path, path, parent_length);
        parent_path[parent_length] = '\0';

        if (!fat_node_lookup(filesystem, parent_path, &parent))
            return FAT_REMOVEDIRECTORY_NOT_FOUND;

        if (!(parent.attributes & 0x10U))
            return FAT_REMOVEDIRECTORY_NOT_DIR;
    }

    fat_node_dir parent_directory;
    if (!fat_node_dir_open(filesystem, &parent, &parent_directory))
        return FAT_REMOVEDIRECTORY_IO_ERROR;

    fat_node_entry found;
    bool matched = false;
    int state;

    while ((state = fat_node_dir_next(&parent_directory, &found)) == FAT_NODE_ENTRY) {
        if (fat_write_delete_name_equal(directory_name, found.name)) {
            matched = true;
            break;
        }
    }

    if (!matched)
        return state == FAT_NODE_ERROR
            ? FAT_REMOVEDIRECTORY_IO_ERROR
            : FAT_REMOVEDIRECTORY_NOT_FOUND;

    if (found.type != FAT_NODE_DIRECTORY ||
        !(found.node.attributes & 0x10U))
        return FAT_REMOVEDIRECTORY_NOT_DIR;

    if (found.node.attributes & 0x01U)
        return FAT_REMOVEDIRECTORY_READ_ONLY;

    if (!fat_write_delete_directory_chain_valid(
            &filesystem->volume, &found.node))
        return FAT_REMOVEDIRECTORY_IO_ERROR;

    fat_node_dir child_directory;
    if (!fat_node_dir_open(filesystem, &found.node, &child_directory))
        return FAT_REMOVEDIRECTORY_IO_ERROR;

    fat_node_entry child;
    while ((state = fat_node_dir_next(&child_directory, &child)) == FAT_NODE_ENTRY) {
        if (!fat_write_delete_dot_entry(child.name))
            return FAT_REMOVEDIRECTORY_NOT_EMPTY;
    }

    if (state == FAT_NODE_ERROR)
        return FAT_REMOVEDIRECTORY_IO_ERROR;

    int result = fat_write_delete_commit(filesystem, &found, true);

    if (result == FAT_DELETE_OK)
        return FAT_REMOVEDIRECTORY_OK;
    if (result == FAT_DELETE_CLEANUP_ERROR)
        return FAT_REMOVEDIRECTORY_CLEANUP_ERROR;

    return FAT_REMOVEDIRECTORY_IO_ERROR;
}


static int fat_write_rename_commit(
    fat_node_fs *filesystem,
    const fat_node_entry *entry,
    const u8 new_name[11]
) {
    fat_write_delete_sector sectors[FAT_WRITE_DELETE_MAX_SECTORS];
    memset(sectors, 0, sizeof(sectors));
    u8 sector_count = 0;

    if (!filesystem || !entry || !new_name ||
        entry->lfn_count > FAT_NODE_LFN_MAX)
        return FAT_RENAME_IO_ERROR;

    for (u32 i = 0; i < entry->lfn_count; ++i) {
        const fat_node_dir_slot *slot = &entry->lfn_slots[i];
        if (!fat_write_delete_mark_slot(
                filesystem,
                sectors,
                &sector_count,
                slot->lba,
                slot->offset,
                slot->raw,
                true,
                false))
            return FAT_RENAME_IO_ERROR;
    }

    int short_index = fat_write_delete_sector_get(
        filesystem, sectors, &sector_count, entry->short_lba
    );

    if (short_index < 0 ||
        entry->short_offset > SECTOR_SIZE - FAT_WRITE_ENTRY_SIZE)
        return FAT_RENAME_IO_ERROR;

    fat_write_delete_sector *short_sector = &sectors[short_index];
    const u8 *current = &short_sector->original[entry->short_offset];

    if (current[0] == 0x00 || current[0] == 0xE5 ||
        current[11] == 0x0F ||
        memcmp(current, entry->raw, FAT_WRITE_ENTRY_SIZE) != 0)
        return FAT_RENAME_IO_ERROR;

    memcpy(
        &short_sector->updated[entry->short_offset],
        new_name,
        11
    );
    short_sector->updated[entry->short_offset + 12] = 0;

    for (u32 i = 0; i < sector_count; ++i) {
        if ((int)i == short_index)
            continue;

        if (!fat_write_delete_write_sector(filesystem, &sectors[i])) {
            fat_write_delete_rollback(filesystem, sectors, sector_count);
            return FAT_RENAME_IO_ERROR;
        }
    }

    if (!fat_write_delete_write_sector(filesystem, &sectors[short_index])) {
        fat_write_delete_rollback(filesystem, sectors, sector_count);
        return FAT_RENAME_IO_ERROR;
    }

    return FAT_RENAME_OK;
}


int fat_node_rename_entry(
    fat_node_fs *filesystem,
    const char *path,
    const char *new_name
) {
    if (!filesystem || !path || !path[0] || !new_name || !new_name[0])
        return FAT_RENAME_INVALID;

    u32 path_length = 0;
    while (path[path_length]) {
        if (path_length >= FAT_NODE_NAME_MAX)
            return FAT_RENAME_INVALID;
        ++path_length;
    }

    u32 split = path_length;
    while (split > 0 && path[split - 1] != '/' && path[split - 1] != '\\')
        --split;

    u32 old_name_length = path_length - split;
    if (old_name_length == 0 || old_name_length > FAT_NODE_NAME_MAX)
        return FAT_RENAME_INVALID;

    char old_name[FAT_NODE_NAME_MAX + 1];
    memcpy(old_name, path + split, old_name_length);
    old_name[old_name_length] = '\0';

    u8 encoded_name[11];
    if (!fat_write_encode_name(new_name, encoded_name))
        return FAT_RENAME_INVALID;

    char parent_path[FAT_NODE_NAME_MAX + 1];
    fat_node parent;
    u32 parent_length = split == 0 ? 0 : split - 1U;

    if (parent_length == 0) {
        if (!fat_node_root(filesystem, &parent))
            return FAT_RENAME_INVALID;
    } else {
        if (parent_length > FAT_NODE_NAME_MAX)
            return FAT_RENAME_INVALID;

        memcpy(parent_path, path, parent_length);
        parent_path[parent_length] = '\0';

        if (!fat_node_lookup(filesystem, parent_path, &parent))
            return FAT_RENAME_NOT_FOUND;
    }

    if (!(parent.attributes & 0x10U))
        return FAT_RENAME_INVALID;

    fat_node_dir directory;
    if (!fat_node_dir_open(filesystem, &parent, &directory))
        return FAT_RENAME_IO_ERROR;

    fat_node_entry found;
    fat_node_entry source_entry;
    memset(&source_entry, 0, sizeof(source_entry));

    bool source_found = false;
    bool conflict_found = false;
    int state;

    while ((state = fat_node_dir_next(&directory, &found)) == FAT_NODE_ENTRY) {
        bool source_match =
            fat_write_delete_name_equal(old_name, found.name);

        bool target_match =
            fat_write_delete_name_equal(new_name, found.name) ||
            memcmp(found.raw, encoded_name, 11) == 0;

        if (source_match) {
            source_entry = found;
            source_found = true;
            continue;
        }

        if (target_match)
            conflict_found = true;
    }

    if (state == FAT_NODE_ERROR)
        return FAT_RENAME_IO_ERROR;

    if (!source_found)
        return FAT_RENAME_NOT_FOUND;

    if (conflict_found)
        return FAT_RENAME_EXISTS;

    if (source_entry.node.attributes & 0x01U)
        return FAT_RENAME_READ_ONLY;

    if (memcmp(source_entry.raw, encoded_name, 11) == 0)
        return FAT_RENAME_OK;

    return fat_write_rename_commit(filesystem, &source_entry, encoded_name);
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

int fat_node_new_directory(fat_node_fs *filesystem, const char *path) {
    if (!filesystem || !path || !path[0])
        return FAT_NEWDIRECTORY_INVALID;

    u32 path_length = 0;
    while (path[path_length]) {
        if (path_length >= FAT_NODE_NAME_MAX)
            return FAT_NEWDIRECTORY_INVALID;
        ++path_length;
    }

    u32 split = path_length;
    while (split > 0 && path[split - 1] != '/' && path[split - 1] != '\\')
        --split;

    u32 name_length = path_length - split;
    if (name_length == 0 || name_length >= FAT_NODE_NAME_MAX)
        return FAT_NEWDIRECTORY_INVALID;

    char directory_name[FAT_NODE_NAME_MAX + 1];
    memcpy(directory_name, path + split, name_length);
    directory_name[name_length] = '\0';

    u8 short_name[11];
    if (!fat_write_encode_name(directory_name, short_name))
        return FAT_NEWDIRECTORY_INVALID;

    fat_node existing;
    if (fat_node_lookup(filesystem, path, &existing))
        return FAT_NEWDIRECTORY_EXISTS;

    char parent_path[FAT_NODE_NAME_MAX + 1];
    fat_node parent;
    u32 parent_length = split == 0 ? 0 : split - 1U;

    if (parent_length == 0) {
        if (!fat_node_root(filesystem, &parent))
            return FAT_NEWDIRECTORY_INVALID;
    } else {
        if (parent_length >= sizeof(parent_path))
            return FAT_NEWDIRECTORY_INVALID;
        memcpy(parent_path, path, parent_length);
        parent_path[parent_length] = '\0';
        if (!fat_node_lookup(filesystem, parent_path, &parent))
            return FAT_NEWDIRECTORY_INVALID;
    }

    if (!(parent.attributes & 0x10U))
        return FAT_NEWDIRECTORY_INVALID;

    fat_write_slot slot;
    int slot_state = fat_write_find_slot(filesystem, &parent, short_name, &slot);
    if (slot_state == FAT_WRITE_IO_ERROR)
        return FAT_NEWDIRECTORY_IO_ERROR;
    if (slot_state == FAT_WRITE_INVALID)
        return FAT_NEWDIRECTORY_INVALID;
    if (slot_state == FAT_WRITE_NO_SPACE && !slot.found)
        return FAT_NEWDIRECTORY_NO_SPACE;
    if (slot.existing)
        return FAT_NEWDIRECTORY_EXISTS;

    u32 new_cluster = 0;
    if (!fat_native_allocate_chain(&filesystem->volume, 1, &new_cluster) || new_cluster < 2)
        return FAT_NEWDIRECTORY_NO_SPACE;

    const u8 self_name[11] = {
        '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    const u8 parent_name[11] = {
        '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
    };
    u32 parent_cluster = parent.fixed_root ? 0 : parent.first_cluster;

    for (u8 sector_index = 0; sector_index < filesystem->volume.sectors_per_cluster; ++sector_index) {
        u8 sector[SECTOR_SIZE];
        memset(sector, 0, sizeof(sector));

        if (sector_index == 0) {
            fat_write_init_directory_entry(
                &sector[0], self_name, new_cluster, &filesystem->volume
            );
            fat_write_init_directory_entry(
                &sector[FAT_WRITE_ENTRY_SIZE], parent_name, parent_cluster,
                &filesystem->volume
            );
        }

        u64 lba;
        if (!fat_native_cluster_sector(&filesystem->volume, new_cluster, sector_index, &lba) ||
            !disk_write_sector(lba, sector, filesystem->volume.disk)) {
            fat_native_release_chain(&filesystem->volume, new_cluster);
            return FAT_NEWDIRECTORY_IO_ERROR;
        }
    }

    int committed = fat_write_commit_directory_entry(
        filesystem, &slot, short_name, new_cluster
    );

    if (committed != 1) {
        if (committed == 0)
            fat_native_release_chain(&filesystem->volume, new_cluster);
        return FAT_NEWDIRECTORY_IO_ERROR;
    }

    return FAT_NEWDIRECTORY_OK;
}
