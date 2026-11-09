#include <types.h>
#include <string.h>
#include <disk.h>
#include "FATNode.h"

static u16 fat_node_u16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

static u32 fat_node_u32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u8 fat_node_short_checksum(const u8 *name) {
    u8 sum = 0;
    for (u32 i = 0; i < 11; ++i)
        sum = (u8)(((sum & 1U) ? 0x80U : 0U) + (sum >> 1) + name[i]);
    return sum;
}

static char fat_node_ascii(u16 value) {
    if (value >= 0x20 && value <= 0x7E)
        return (char)value;
    return '?';
}

static char fat_node_lower(char value) {
    if (value >= 'A' && value <= 'Z')
        return (char)(value - 'A' + 'a');
    return value;
}

static char fat_node_upper(char value) {
    if (value >= 'a' && value <= 'z')
        return (char)(value - 'a' + 'A');
    return value;
}

static bool fat_node_equal(const char *a, const char *b) {
    u32 i = 0;
    while (a[i] && b[i]) {
        if (fat_node_upper(a[i]) != fat_node_upper(b[i]))
            return false;
        ++i;
    }
    return a[i] == b[i];
}

static void fat_node_lfn_reset(fat_node_dir *directory) {
    directory->lfn_active = 0;
    directory->lfn_expected = 0;
    directory->lfn_checksum = 0;
}

static void fat_node_lfn_copy(fat_node_dir *directory, const u8 *entry, u8 sequence) {
    static const u8 positions[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    u32 base = ((u32)sequence - 1U) * 13U;
    for (u32 i = 0; i < 13; ++i)
        directory->lfn_units[base + i] = fat_node_u16(&entry[positions[i]]);
}

static void fat_node_lfn_feed(fat_node_dir *directory, const u8 *entry) {
    u8 sequence = entry[0] & 0x1FU;
    bool last = (entry[0] & 0x40U) != 0;
    if (sequence == 0 || sequence > 20) {
        fat_node_lfn_reset(directory);
        return;
    }
    if (last) {
        for (u32 i = 0; i < 260; ++i)
            directory->lfn_units[i] = 0xFFFF;
        directory->lfn_active = 1;
        directory->lfn_expected = sequence;
        directory->lfn_checksum = entry[13];
    } else if (!directory->lfn_active || directory->lfn_checksum != entry[13] || directory->lfn_expected != sequence) {
        fat_node_lfn_reset(directory);
        return;
    }
    if (!directory->lfn_active || directory->lfn_checksum != entry[13] || directory->lfn_expected != sequence) {
        fat_node_lfn_reset(directory);
        return;
    }
    fat_node_lfn_copy(directory, entry, sequence);
    directory->lfn_expected = (u8)(sequence - 1U);
}

static bool fat_node_lfn_decode(const fat_node_dir *directory, const u8 *short_entry, char output[256]) {
    if (!directory->lfn_active || directory->lfn_expected != 0 ||
        directory->lfn_checksum != fat_node_short_checksum(short_entry))
        return false;
    u32 out = 0;
    for (u32 i = 0; i < 260 && out < FAT_NODE_NAME_MAX; ++i) {
        u16 value = directory->lfn_units[i];
        if (value == 0 || value == 0xFFFF)
            break;
        output[out++] = fat_node_ascii(value);
    }
    if (out == 0)
        return false;
    output[out] = '\0';
    return true;
}

static void fat_node_short_decode(const u8 *entry, char output[256]) {
    u32 out = 0;
    for (u32 i = 0; i < 8 && entry[i] != ' '; ++i) {
        u8 value = entry[i];
        if (i == 0 && value == 0x05)
            value = 0xE5;
        char c = fat_node_ascii(value);
        if (entry[12] & 0x08)
            c = fat_node_lower(c);
        output[out++] = c;
    }
    bool extension = false;
    for (u32 i = 0; i < 3; ++i) {
        if (entry[8 + i] != ' ')
            extension = true;
    }
    if (extension) {
        output[out++] = '.';
        for (u32 i = 0; i < 3 && entry[8 + i] != ' '; ++i) {
            char c = fat_node_ascii(entry[8 + i]);
            if (entry[12] & 0x10)
                c = fat_node_lower(c);
            output[out++] = c;
        }
    }
    output[out] = '\0';
}

static void fat_node_entry_build(fat_node_dir *directory, const u8 *raw, fat_node_entry *entry) {
    u32 low = fat_node_u16(&raw[26]);
    u32 high = fat_node_u16(&raw[20]);
    entry->node.first_cluster = (directory->filesystem->volume.fat_bits == FAT_NATIVE_FAT32)
        ? ((high << 16) | low) & 0x0FFFFFFFU
        : low;
    entry->node.size = fat_node_u32(&raw[28]);
    entry->node.attributes = raw[11];
    entry->node.is_root = 0;
    entry->node.fixed_root = 0;
    entry->node.reserved = 0;
    entry->type = (raw[11] & 0x10U) ? FAT_NODE_DIRECTORY : FAT_NODE_FILE;
    if (!fat_node_lfn_decode(directory, raw, entry->name))
        fat_node_short_decode(raw, entry->name);
    fat_node_lfn_reset(directory);
}

bool fat_node_mount(fat_node_fs *filesystem, unsigned int disk, unsigned int partition) {
    if (!filesystem || !fat_native_mount(&filesystem->volume, disk, partition))
        return false;
    u8 boot[SECTOR_SIZE];
    if (!disk_read_sector(filesystem->volume.partition_lba, boot, disk))
        return false;
    filesystem->root_entries = fat_node_u16(&boot[17]);
    if (filesystem->volume.fat_bits == FAT_NATIVE_FAT32)
        return filesystem->root_entries == 0 && filesystem->volume.root_cluster >= 2;
    return filesystem->root_entries != 0 &&
        filesystem->root_entries <= filesystem->volume.root_sectors * 16U;
}

bool fat_node_root(const fat_node_fs *filesystem, fat_node *node) {
    if (!filesystem || !node)
        return false;
    node->first_cluster = filesystem->volume.root_cluster;
    node->size = 0;
    node->attributes = 0x10;
    node->is_root = 1;
    node->fixed_root = filesystem->volume.fat_bits == FAT_NATIVE_FAT32 ? 0 : 1;
    node->reserved = 0;
    return true;
}

bool fat_node_dir_open(const fat_node_fs *filesystem, const fat_node *node, fat_node_dir *directory) {
    if (!filesystem || !node || !directory || !(node->attributes & 0x10U))
        return false;
    if (!node->fixed_root && (node->first_cluster < 2 || node->first_cluster > filesystem->volume.cluster_count + 1))
        return false;
    memset(directory, 0, sizeof(*directory));
    directory->filesystem = filesystem;
    directory->directory = *node;
    directory->current_cluster = node->first_cluster;
    return true;
}

static int fat_node_dir_load_sector(fat_node_dir *directory) {
    const fat_native_volume *volume = &directory->filesystem->volume;
    u64 lba;
    if (directory->directory.fixed_root) {
        if (directory->entry_index >= directory->filesystem->root_entries)
            return FAT_NODE_END;
        u32 sector_number = directory->entry_index / 16U;
        if (sector_number >= volume->root_sectors)
            return FAT_NODE_ERROR;
        lba = volume->partition_lba + volume->root_start + sector_number;
    } else {
        if (directory->sector_index >= volume->sectors_per_cluster) {
            u32 next = 0;
            int state = fat_native_chain_next(volume, directory->current_cluster, &next);
            if (state == FAT_NATIVE_CHAIN_END)
                return FAT_NODE_END;
            if (state != FAT_NATIVE_CHAIN_NEXT || ++directory->cluster_steps >= volume->cluster_count)
                return FAT_NODE_ERROR;
            directory->current_cluster = next;
            directory->sector_index = 0;
        }
        if (!fat_native_cluster_sector(volume, directory->current_cluster, directory->sector_index, &lba))
            return FAT_NODE_ERROR;
    }
    if (!disk_read_sector(lba, directory->sector, volume->disk))
        return FAT_NODE_ERROR;
    directory->buffer_valid = 1;
    return FAT_NODE_ENTRY;
}

int fat_node_dir_next(fat_node_dir *directory, fat_node_entry *entry) {
    if (!directory || !entry || !directory->filesystem || directory->ended)
        return FAT_NODE_END;
    for (;;) {
        if (!directory->buffer_valid) {
            int loaded = fat_node_dir_load_sector(directory);
            if (loaded == FAT_NODE_END) {
                directory->ended = 1;
                return FAT_NODE_END;
            }
            if (loaded != FAT_NODE_ENTRY)
                return FAT_NODE_ERROR;
        }
        u8 *raw = &directory->sector[directory->entry_offset];
        directory->entry_offset = (u16)(directory->entry_offset + 32U);
        ++directory->entry_index;
        if (directory->entry_offset >= SECTOR_SIZE) {
            directory->entry_offset = 0;
            directory->buffer_valid = 0;
            if (!directory->directory.fixed_root)
                ++directory->sector_index;
        }
        if (raw[0] == 0x00) {
            directory->ended = 1;
            fat_node_lfn_reset(directory);
            return FAT_NODE_END;
        }
        if (raw[0] == 0xE5) {
            fat_node_lfn_reset(directory);
            continue;
        }
        if (raw[11] == 0x0F) {
            fat_node_lfn_feed(directory, raw);
            continue;
        }
        if (raw[11] & 0x08U) {
            fat_node_lfn_reset(directory);
            continue;
        }
        fat_node_entry_build(directory, raw, entry);
        if (entry->name[0] == '\0')
            continue;
        if (entry->type == FAT_NODE_DIRECTORY && entry->node.first_cluster < 2)
            return FAT_NODE_ERROR;
        if (entry->type == FAT_NODE_FILE && entry->node.size != 0 && entry->node.first_cluster < 2)
            return FAT_NODE_ERROR;
        if (entry->type == FAT_NODE_FILE && entry->node.size == 0 && entry->node.first_cluster != 0)
            return FAT_NODE_ERROR;
        return FAT_NODE_ENTRY;
    }
}

bool fat_node_lookup(const fat_node_fs *filesystem, const char *path, fat_node *node) {
    if (!filesystem || !path || !node)
        return false;
    fat_node current;
    if (!fat_node_root(filesystem, &current))
        return false;
    const char *cursor = path;
    while (*cursor == '/' || *cursor == '\\')
        ++cursor;
    if (*cursor == '\0') {
        *node = current;
        return true;
    }
    for (;;) {
        char component[256];
        u32 length = 0;
        while (*cursor && *cursor != '/' && *cursor != '\\') {
            if (length >= FAT_NODE_NAME_MAX)
                return false;
            component[length++] = *cursor++;
        }
        if (length == 0)
            return false;
        component[length] = '\0';
        if (!(current.attributes & 0x10U))
            return false;
        fat_node_dir iterator;
        if (!fat_node_dir_open(filesystem, &current, &iterator))
            return false;
        fat_node_entry found;
        bool matched = false;
        int state;
        while ((state = fat_node_dir_next(&iterator, &found)) == FAT_NODE_ENTRY) {
            if (fat_node_equal(component, found.name)) {
                current = found.node;
                matched = true;
                break;
            }
        }
        if (!matched || state == FAT_NODE_ERROR)
            return false;
        while (*cursor == '/' || *cursor == '\\')
            ++cursor;
        if (*cursor == '\0') {
            *node = current;
            return true;
        }
    }
}

static bool fat_node_validate_chain(const fat_native_volume *volume, u32 first_cluster, u64 required_clusters) {
    if (required_clusters == 0)
        return first_cluster == 0;
    if (first_cluster < 2 || first_cluster > volume->cluster_count + 1 || required_clusters > volume->cluster_count)
        return false;
    u32 current = first_cluster;
    u32 slow = first_cluster;
    u32 fast = first_cluster;
    bool fast_active = true;
    for (u64 step = 0; step < required_clusters; ++step) {
        if (current < 2 || current > volume->cluster_count + 1)
            return false;
        if (step + 1 == required_clusters)
            return true;
        u32 next_current = 0;
        if (fat_native_chain_next(volume, current, &next_current) != FAT_NATIVE_CHAIN_NEXT)
            return false;
        current = next_current;
        u32 next_slow = 0;
        if (fat_native_chain_next(volume, slow, &next_slow) == FAT_NATIVE_CHAIN_NEXT)
            slow = next_slow;
        else
            slow = 0;
        if (fast_active) {
            u32 first_fast = 0;
            if (fat_native_chain_next(volume, fast, &first_fast) != FAT_NATIVE_CHAIN_NEXT) {
                fast_active = false;
            } else {
                u32 second_fast = 0;
                if (fat_native_chain_next(volume, first_fast, &second_fast) == FAT_NATIVE_CHAIN_NEXT)
                    fast = second_fast;
                else
                    fast_active = false;
            }
        }
        if (fast_active && slow != 0 && slow == fast)
            return false;
    }
    return false;
}

bool fat_node_read(const fat_node_fs *filesystem, const fat_node *node, u64 offset, void *buffer, size_t count, size_t *read_count) {
    if (read_count)
        *read_count = 0;
    if (!filesystem || !node || !read_count || (!buffer && count != 0) || (node->attributes & 0x10U) || node->is_root)
        return false;
    if (count == 0 || offset >= node->size)
        return true;
    u64 remaining_file = (u64)node->size - offset;
    if ((u64)count > remaining_file)
        count = (size_t)remaining_file;
    if (node->size == 0)
        return true;
    const fat_native_volume *volume = &filesystem->volume;
    u32 cluster_bytes = (u32)volume->sectors_per_cluster * volume->bytes_per_sector;
    if (cluster_bytes == 0)
        return false;
    u64 required_clusters = ((u64)node->size + cluster_bytes - 1U) / cluster_bytes;
    if (!fat_node_validate_chain(volume, node->first_cluster, required_clusters))
        return false;
    u64 cluster_index = offset / cluster_bytes;
    u32 cluster_offset = (u32)(offset % cluster_bytes);
    u32 current_cluster = node->first_cluster;
    for (u64 step = 0; step < cluster_index; ++step) {
        u32 next = 0;
        if (step >= volume->cluster_count || fat_native_chain_next(volume, current_cluster, &next) != FAT_NATIVE_CHAIN_NEXT)
            return false;
        current_cluster = next;
    }
    u8 sector[SECTOR_SIZE];
    u8 *output = buffer;
    size_t completed = 0;
    while (completed < count) {
        u32 sector_index = cluster_offset / volume->bytes_per_sector;
        u32 sector_offset = cluster_offset % volume->bytes_per_sector;
        u64 lba;
        if (!fat_native_cluster_sector(volume, current_cluster, (u8)sector_index, &lba) ||
            !disk_read_sector(lba, sector, volume->disk))
            return false;
        size_t available = volume->bytes_per_sector - sector_offset;
        size_t chunk = count - completed;
        if (chunk > available)
            chunk = available;
        memcpy(output + completed, sector + sector_offset, chunk);
        completed += chunk;
        cluster_offset += (u32)chunk;
        if (completed < count && cluster_offset >= cluster_bytes) {
            u32 next = 0;
            if (fat_native_chain_next(volume, current_cluster, &next) != FAT_NATIVE_CHAIN_NEXT ||
                completed / cluster_bytes >= volume->cluster_count)
                return false;
            current_cluster = next;
            cluster_offset = 0;
        }
    }
    *read_count = completed;
    return true;
}
