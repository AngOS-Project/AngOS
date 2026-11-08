#include <types.h>
#include <string.h>
#include <disk.h>
#include "FATCore.h"

static u16 fat_native_u16(const u8 *data) {
    return (u16)data[0] | ((u16)data[1] << 8);
}

static u32 fat_native_u32(const u8 *data) {
    return (u32)data[0] |
        ((u32)data[1] << 8) |
        ((u32)data[2] << 16) |
        ((u32)data[3] << 24);
}

static u32 fat_native_last_cluster(const fat_native_volume *volume) {
    return volume->cluster_count + 1;
}

static u32 fat_native_eoc(const fat_native_volume *volume) {
    if (volume->fat_bits == FAT_NATIVE_FAT12)
        return 0x0FFF;
    if (volume->fat_bits == FAT_NATIVE_FAT16)
        return 0xFFFF;
    return 0x0FFFFFFF;
}

static u32 fat_native_eoc_floor(const fat_native_volume *volume) {
    if (volume->fat_bits == FAT_NATIVE_FAT12)
        return 0x0FF8;
    if (volume->fat_bits == FAT_NATIVE_FAT16)
        return 0xFFF8;
    return 0x0FFFFFF8;
}

static u32 fat_native_reserved_floor(const fat_native_volume *volume) {
    if (volume->fat_bits == FAT_NATIVE_FAT12)
        return 0x0FF0;
    if (volume->fat_bits == FAT_NATIVE_FAT16)
        return 0xFFF0;
    return 0x0FFFFFF0;
}

static u32 fat_native_bad_value(const fat_native_volume *volume) {
    if (volume->fat_bits == FAT_NATIVE_FAT12)
        return 0x0FF7;
    if (volume->fat_bits == FAT_NATIVE_FAT16)
        return 0xFFF7;
    return 0x0FFFFFF7;
}

static u8 fat_native_read_copy(const fat_native_volume *volume) {
    return volume->mirrored ? 0 : volume->active_fat;
}

static bool fat_native_read_bytes(
    const fat_native_volume *volume,
    u8 copy,
    u32 offset,
    u8 count,
    u8 *output
) {
    if (!volume || !output || copy >= volume->fat_count || count == 0 || count > 4)
        return false;

    u64 fat_bytes = (u64)volume->fat_sectors * volume->bytes_per_sector;
    if ((u64)offset + count > fat_bytes)
        return false;

    u64 base = volume->partition_lba + volume->fat_start + (u64)copy * volume->fat_sectors;
    u32 first_sector = offset / volume->bytes_per_sector;
    u32 first_offset = offset % volume->bytes_per_sector;
    u8 first[SECTOR_SIZE];
    u8 second[SECTOR_SIZE];

    if (!disk_read_sector(base + first_sector, first, volume->disk))
        return false;

    bool second_loaded = false;
    for (u8 i = 0; i < count; ++i) {
        u32 position = first_offset + i;
        if (position < volume->bytes_per_sector) {
            output[i] = first[position];
        } else {
            if (!second_loaded) {
                if (first_sector + 1 >= volume->fat_sectors ||
                    !disk_read_sector(base + first_sector + 1, second, volume->disk))
                    return false;
                second_loaded = true;
            }
            output[i] = second[position - volume->bytes_per_sector];
        }
    }

    return true;
}

static bool fat_native_write_bytes(
    const fat_native_volume *volume,
    u8 copy,
    u32 offset,
    u8 count,
    const u8 *input
) {
    if (!volume || !input || copy >= volume->fat_count || count == 0 || count > 4)
        return false;

    u64 fat_bytes = (u64)volume->fat_sectors * volume->bytes_per_sector;
    if ((u64)offset + count > fat_bytes)
        return false;

    u64 base = volume->partition_lba + volume->fat_start + (u64)copy * volume->fat_sectors;
    u32 first_sector = offset / volume->bytes_per_sector;
    u32 first_offset = offset % volume->bytes_per_sector;
    u8 first[SECTOR_SIZE];
    u8 second[SECTOR_SIZE];

    if (!disk_read_sector(base + first_sector, first, volume->disk))
        return false;

    bool needs_second = first_offset + count > volume->bytes_per_sector;
    if (needs_second) {
        if (first_sector + 1 >= volume->fat_sectors ||
            !disk_read_sector(base + first_sector + 1, second, volume->disk))
            return false;
    }

    for (u8 i = 0; i < count; ++i) {
        u32 position = first_offset + i;
        if (position < volume->bytes_per_sector)
            first[position] = input[i];
        else
            second[position - volume->bytes_per_sector] = input[i];
    }

    if (!disk_write_sector(base + first_sector, first, volume->disk))
        return false;
    if (needs_second && !disk_write_sector(base + first_sector + 1, second, volume->disk))
        return false;

    u8 verify[4];
    if (!fat_native_read_bytes(volume, copy, offset, count, verify))
        return false;

    for (u8 i = 0; i < count; ++i) {
        if (verify[i] != input[i])
            return false;
    }

    return true;
}

bool fat_native_mount(fat_native_volume *volume, unsigned int disk, unsigned int partition) {
    if (!volume || !disktable || disk >= 4 || partition >= 4)
        return false;

    partinfo *part = &disktable[disk].parts[partition];
    if (part->loc == 0 || part->size == 0 ||
        part->loc > 0xFFFFFFFFFFFFFFFFULL - part->size)
        return false;

    u64 partition_end = part->loc + part->size;
    if (disktable[disk].blocks != 0 && partition_end > disktable[disk].blocks)
        return false;

    u8 boot[SECTOR_SIZE];
    if (!disk_read_sector(part->loc, boot, disk))
        return false;

    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return false;

    u16 bytes_per_sector = fat_native_u16(&boot[11]);
    u8 sectors_per_cluster = boot[13];
    u16 reserved_sectors = fat_native_u16(&boot[14]);
    u8 fat_count = boot[16];
    u16 root_entries = fat_native_u16(&boot[17]);
    u16 total_sectors_16 = fat_native_u16(&boot[19]);
    u16 fat_sectors_16 = fat_native_u16(&boot[22]);
    u32 total_sectors_32 = fat_native_u32(&boot[32]);
    u32 fat_sectors_32 = fat_native_u32(&boot[36]);
    u32 total_sectors = total_sectors_16 ? total_sectors_16 : total_sectors_32;
    u32 fat_sectors = fat_sectors_16 ? fat_sectors_16 : fat_sectors_32;

    if (bytes_per_sector != SECTOR_SIZE || sectors_per_cluster == 0 ||
        sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 || fat_count == 0 || fat_count > 4 ||
        total_sectors == 0 || fat_sectors == 0 || total_sectors > part->size)
        return false;

    u64 fat_region_end = (u64)reserved_sectors + (u64)fat_count * fat_sectors;
    u64 root_sectors = ((u64)root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;
    u64 data_start = fat_region_end + root_sectors;

    if (fat_region_end >= total_sectors || data_start >= total_sectors ||
        data_start > 0xFFFFFFFFULL)
        return false;

    u32 cluster_count = (u32)((total_sectors - data_start) / sectors_per_cluster);
    if (cluster_count == 0 || cluster_count > 0x0FFFFFF0U)
        return false;

    u8 fat_bits;
    if (cluster_count < 4085)
        fat_bits = FAT_NATIVE_FAT12;
    else if (cluster_count < 65525)
        fat_bits = FAT_NATIVE_FAT16;
    else
        fat_bits = FAT_NATIVE_FAT32;

    if ((fat_bits == FAT_NATIVE_FAT32 && (root_entries != 0 || fat_sectors_16 != 0 || fat_sectors_32 == 0)) ||
        (fat_bits != FAT_NATIVE_FAT32 && (root_entries == 0 || fat_sectors_16 == 0)))
        return false;

    u32 last_cluster = cluster_count + 1;
    if ((fat_bits == FAT_NATIVE_FAT12 && last_cluster >= 0x0FF0U) ||
        (fat_bits == FAT_NATIVE_FAT16 && last_cluster >= 0xFFF0U) ||
        (fat_bits == FAT_NATIVE_FAT32 && last_cluster >= 0x0FFFFFF0U))
        return false;

    u64 fat_bytes = (u64)fat_sectors * bytes_per_sector;
    u64 last_offset;
    if (fat_bits == FAT_NATIVE_FAT12)
        last_offset = ((u64)last_cluster * 3) / 2;
    else if (fat_bits == FAT_NATIVE_FAT16)
        last_offset = (u64)last_cluster * 2;
    else
        last_offset = (u64)last_cluster * 4;

    if (last_offset + (fat_bits == FAT_NATIVE_FAT32 ? 3 : 1) >= fat_bytes)
        return false;

    u32 root_cluster = 0;
    u32 fsinfo_sector = 0xFFFFFFFFU;
    bool mirrored = true;
    u8 active_fat = 0;

    if (fat_bits == FAT_NATIVE_FAT32) {
        u16 flags = fat_native_u16(&boot[40]);
        root_cluster = fat_native_u32(&boot[44]) & 0x0FFFFFFFU;
        u16 fsinfo_value = fat_native_u16(&boot[48]);
        if (root_cluster < 2 || root_cluster > last_cluster)
            return false;
        if (fsinfo_value > 0 && fsinfo_value < reserved_sectors)
            fsinfo_sector = fsinfo_value;
        if (flags & 0x0080U) {
            mirrored = false;
            active_fat = (u8)(flags & 0x000FU);
            if (active_fat >= fat_count)
                return false;
        }
    }

    memset(volume, 0, sizeof(*volume));
    volume->disk = disk;
    volume->partition = partition;
    volume->partition_lba = part->loc;
    volume->partition_sectors = part->size;
    volume->volume_sectors = total_sectors;
    volume->fat_start = reserved_sectors;
    volume->fat_sectors = fat_sectors;
    volume->root_start = (u32)fat_region_end;
    volume->root_sectors = (u32)root_sectors;
    volume->data_start = (u32)data_start;
    volume->cluster_count = cluster_count;
    volume->root_cluster = root_cluster;
    volume->fsinfo_sector = fsinfo_sector;
    volume->allocation_hint = 2;
    volume->bytes_per_sector = bytes_per_sector;
    volume->sectors_per_cluster = sectors_per_cluster;
    volume->fat_count = fat_count;
    volume->fat_bits = fat_bits;
    volume->mirrored = mirrored ? 1 : 0;
    volume->active_fat = active_fat;

    return true;
}

bool fat_native_cluster_sector(const fat_native_volume *volume, u32 cluster, u8 sector_index, u64 *lba) {
    if (!volume || !lba || cluster < 2 || cluster > fat_native_last_cluster(volume) ||
        sector_index >= volume->sectors_per_cluster)
        return false;

    u64 relative = (u64)volume->data_start +
        (u64)(cluster - 2) * volume->sectors_per_cluster + sector_index;
    if (relative >= volume->volume_sectors)
        return false;

    *lba = volume->partition_lba + relative;
    return true;
}

bool fat_native_fat_read(const fat_native_volume *volume, u32 cluster, u32 *value) {
    if (!volume || !value || cluster < 2 || cluster > fat_native_last_cluster(volume))
        return false;

    u8 copy = fat_native_read_copy(volume);
    u8 bytes[4] = {0, 0, 0, 0};
    u32 offset;
    u8 count;

    if (volume->fat_bits == FAT_NATIVE_FAT12) {
        offset = (u32)(((u64)cluster * 3) / 2);
        count = 2;
    } else if (volume->fat_bits == FAT_NATIVE_FAT16) {
        offset = cluster * 2;
        count = 2;
    } else if (volume->fat_bits == FAT_NATIVE_FAT32) {
        offset = cluster * 4;
        count = 4;
    } else {
        return false;
    }

    if (!fat_native_read_bytes(volume, copy, offset, count, bytes))
        return false;

    u32 raw = (u32)bytes[0] | ((u32)bytes[1] << 8);
    if (count == 4)
        raw |= ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);

    if (volume->fat_bits == FAT_NATIVE_FAT12)
        *value = (cluster & 1U) ? (raw >> 4) & 0x0FFFU : raw & 0x0FFFU;
    else if (volume->fat_bits == FAT_NATIVE_FAT16)
        *value = raw & 0xFFFFU;
    else
        *value = raw & 0x0FFFFFFFU;

    return true;
}

bool fat_native_fat_write(const fat_native_volume *volume, u32 cluster, u32 value) {
    if (!volume || cluster < 2 || cluster > fat_native_last_cluster(volume))
        return false;

    u8 count;
    u32 offset;
    u8 bytes[4] = {0, 0, 0, 0};
    u32 max_value = fat_native_eoc(volume);

    if (value > max_value)
        return false;

    if (volume->fat_bits == FAT_NATIVE_FAT12) {
        offset = (u32)(((u64)cluster * 3) / 2);
        count = 2;
    } else if (volume->fat_bits == FAT_NATIVE_FAT16) {
        offset = cluster * 2;
        count = 2;
    } else if (volume->fat_bits == FAT_NATIVE_FAT32) {
        offset = cluster * 4;
        count = 4;
    } else {
        return false;
    }

    u8 copies[4];
    u8 copy_count = 0;
    if (volume->mirrored) {
        for (u8 i = 0; i < volume->fat_count; ++i)
            copies[copy_count++] = i;
    } else {
        if (volume->active_fat >= volume->fat_count)
            return false;
        copies[copy_count++] = volume->active_fat;
    }

    bool all_ok = true;
    for (u8 i = 0; i < copy_count; ++i) {
        u32 raw = 0;
        u8 old_bytes[4] = {0, 0, 0, 0};
        if (!fat_native_read_bytes(volume, copies[i], offset, count, old_bytes)) {
            all_ok = false;
            continue;
        }
        raw = (u32)old_bytes[0] | ((u32)old_bytes[1] << 8);
        if (count == 4)
            raw |= ((u32)old_bytes[2] << 16) | ((u32)old_bytes[3] << 24);

        if (volume->fat_bits == FAT_NATIVE_FAT12) {
            if (cluster & 1U)
                raw = (raw & 0x000FU) | ((value & 0x0FFFU) << 4);
            else
                raw = (raw & 0xF000U) | (value & 0x0FFFU);
        } else if (volume->fat_bits == FAT_NATIVE_FAT16) {
            raw = value & 0xFFFFU;
        } else {
            raw = (raw & 0xF0000000U) | (value & 0x0FFFFFFFU);
        }

        bytes[0] = (u8)raw;
        bytes[1] = (u8)(raw >> 8);
        bytes[2] = (u8)(raw >> 16);
        bytes[3] = (u8)(raw >> 24);
        if (!fat_native_write_bytes(volume, copies[i], offset, count, bytes))
            all_ok = false;
    }

    return all_ok;
}

int fat_native_chain_next(const fat_native_volume *volume, u32 cluster, u32 *next_cluster) {
    if (!volume || !next_cluster || cluster < 2 || cluster > fat_native_last_cluster(volume))
        return FAT_NATIVE_CHAIN_ERROR;

    u32 value;
    if (!fat_native_fat_read(volume, cluster, &value))
        return FAT_NATIVE_CHAIN_ERROR;

    if (value >= fat_native_eoc_floor(volume)) {
        *next_cluster = 0;
        return FAT_NATIVE_CHAIN_END;
    }

    if (value < 2 || value > fat_native_last_cluster(volume) ||
        value == fat_native_bad_value(volume) || value >= fat_native_reserved_floor(volume))
        return FAT_NATIVE_CHAIN_ERROR;

    *next_cluster = value;
    return FAT_NATIVE_CHAIN_NEXT;
}

bool fat_native_find_free(fat_native_volume *volume, u32 *cluster) {
    if (!volume || !cluster || volume->cluster_count == 0)
        return false;

    u32 first = volume->allocation_hint;
    if (first < 2 || first > fat_native_last_cluster(volume))
        first = 2;

    u32 candidate = first;
    for (u32 scanned = 0; scanned < volume->cluster_count; ++scanned) {
        u32 value;
        if (!fat_native_fat_read(volume, candidate, &value))
            return false;
        if (value == 0) {
            *cluster = candidate;
            volume->allocation_hint = candidate == fat_native_last_cluster(volume) ? 2 : candidate + 1;
            return true;
        }
        candidate = candidate == fat_native_last_cluster(volume) ? 2 : candidate + 1;
    }

    return false;
}

bool fat_native_release_chain(const fat_native_volume *volume, u32 first_cluster) {
    if (!volume)
        return false;
    if (first_cluster == 0)
        return true;
    if (first_cluster < 2 || first_cluster > fat_native_last_cluster(volume))
        return false;

    u32 current = first_cluster;
    bool terminated = false;

    for (u32 step = 0; step < volume->cluster_count; ++step) {
        u32 next;
        int state = fat_native_chain_next(volume, current, &next);
        if (state == FAT_NATIVE_CHAIN_ERROR)
            return false;
        if (state == FAT_NATIVE_CHAIN_END) {
            terminated = true;
            break;
        }
        current = next;
    }

    if (!terminated)
        return false;

    current = first_cluster;
    for (u32 step = 0; step < volume->cluster_count; ++step) {
        u32 next;
        int state = fat_native_chain_next(volume, current, &next);
        if (state == FAT_NATIVE_CHAIN_ERROR)
            return false;
        if (!fat_native_fat_write(volume, current, 0))
            return false;
        if (state == FAT_NATIVE_CHAIN_END)
            return true;
        current = next;
    }

    return false;
}

bool fat_native_allocate_chain(fat_native_volume *volume, u32 count, u32 *first_cluster) {
    if (!volume || !first_cluster)
        return false;

    *first_cluster = 0;
    if (count == 0)
        return true;
    if (count > volume->cluster_count)
        return false;

    u32 first = 0;
    u32 previous = 0;
    u32 eoc = fat_native_eoc(volume);

    for (u32 i = 0; i < count; ++i) {
        u32 current;
        if (!fat_native_find_free(volume, &current)) {
            if (first)
                fat_native_release_chain(volume, first);
            return false;
        }

        if (!fat_native_fat_write(volume, current, eoc)) {
            fat_native_fat_write(volume, current, 0);
            if (first)
                fat_native_release_chain(volume, first);
            return false;
        }

        if (previous && !fat_native_fat_write(volume, previous, current)) {
            fat_native_fat_write(volume, previous, eoc);
            fat_native_fat_write(volume, current, 0);
            if (first)
                fat_native_release_chain(volume, first);
            return false;
        }

        if (!first)
            first = current;
        previous = current;
    }

    *first_cluster = first;
    return true;
}
