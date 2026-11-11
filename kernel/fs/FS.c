#include <types.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <FS.h>
#include "FAT.h"
#include "FATNode.h"
#include "FATWrite.h"

#define MAX_OPEN_DIRS 32
#define MAX_PATH_COMPONENTS 15

static FILE file_table[MAX_OPEN_FILES];
FILE *files = file_table;

static DIR dir_table[MAX_OPEN_DIRS];

typedef struct {
    fat_node_fs filesystem;
    unsigned int disk;
    unsigned int partition;
    u64 location;
    u64 sectors;
    u8 type;
    u8 valid;
} fs_native_cache;

static fs_native_cache native_cache[LETTERS];

static int fs_dir_slot_status(const DIR *dir) {
    if (!dir)
        return 1;
    u64 address = (u64)dir;
    u64 base = (u64)dir_table;
    u64 table_size = (u64)sizeof(DIR) * MAX_OPEN_DIRS;
    if (address < base)
        return 1;
    u64 offset = address - base;
    if (offset >= table_size)
        return 1;
    if (offset % sizeof(DIR) != 0)
        return 2;
    return 0;
}

static bool fs_dir_is_open(const DIR *dir) {
    return fs_dir_slot_status(dir) == 0 && dir->used != 0 && dir->volume < LETTERS;
}

static int fs_file_slot_status(const FILE *fp) {
    if (!fp || !files)
        return 1;
    u64 address = (u64)fp;
    u64 base = (u64)files;
    u64 table_size = (u64)sizeof(FILE) * MAX_OPEN_FILES;
    if (address < base)
        return 1;
    u64 offset = address - base;
    if (offset >= table_size)
        return 1;
    if (offset % sizeof(FILE) != 0)
        return 2;
    return 0;
}

static bool fs_file_is_open(const FILE *fp) {
    if (fs_file_slot_status(fp) != 0)
        return false;
    return fp->used != 0 && fp->path != 0 && fp->path_count != 0;
}

static bool fs_parse_volume_path(const char *path, u8 *volume, const char **relative) {
    if (!path || !volume || !relative)
        return false;
    u32 length = 0;
    while (path[length]) {
        if (length >= FILENAME_LENGTH - 1)
            return false;
        ++length;
    }
    if (length < 3 || path[0] != '|' || path[1] < 'A' || path[1] > 'Z' || path[2] != '/')
        return false;
    *volume = (u8)(path[1] - 'A');
    *relative = path + 3;
    return *volume < LETTERS;
}

static bool fs_split_path(const char *path, char ***components, u32 *component_count) {
    if (!path || !components || !component_count || !path[0])
        return false;
    u32 count = 0;
    const char *cursor = path;
    while (*cursor) {
        if (*cursor == '/' || *cursor == '\\')
            return false;
        u32 length = 0;
        while (cursor[length] && cursor[length] != '/' && cursor[length] != '\\') {
            if (++length >= FILENAME_LENGTH)
                return false;
        }
        if (length == 0 || ++count > MAX_PATH_COMPONENTS)
            return false;
        cursor += length;
        if (*cursor == '/' || *cursor == '\\') {
            ++cursor;
            if (!*cursor)
                return false;
        }
    }
    char **parts = malloc((count + 1U) * sizeof(char *));
    if (!parts)
        return false;
    for (u32 i = 0; i <= count; ++i)
        parts[i] = 0;
    cursor = path;
    for (u32 i = 0; i < count; ++i) {
        u32 length = 0;
        while (cursor[length] && cursor[length] != '/' && cursor[length] != '\\')
            ++length;
        parts[i] = malloc(length + 1U);
        if (!parts[i]) {
            for (u32 j = 0; j < count; ++j)
                free(parts[j]);
            free(parts);
            return false;
        }
        memcpy(parts[i], cursor, length);
        parts[i][length] = '\0';
        cursor += length;
        if (*cursor == '/' || *cursor == '\\')
            ++cursor;
    }
    parts[count] = 0;
    *components = parts;
    *component_count = count;
    return true;
}

static void fs_free_path(char **path, u32 count) {
    if (!path)
        return;
    for (u32 i = 0; i < count; ++i) {
        if (path[i])
            free(path[i]);
    }
    free(path);
}

static bool fs_join_file_path(const FILE *fp, char output[FILENAME_LENGTH]) {
    if (!fp || !fp->path || fp->path_count == 0 || fp->path_count > MAX_PATH_COMPONENTS)
        return false;
    u32 used = 0;
    for (u32 i = 0; i < fp->path_count; ++i) {
        if (!fp->path[i] || !fp->path[i][0])
            return false;
        if (i != 0) {
            if (used >= FILENAME_LENGTH - 1)
                return false;
            output[used++] = '/';
        }
        for (u32 j = 0; fp->path[i][j]; ++j) {
            if (used >= FILENAME_LENGTH - 1)
                return false;
            output[used++] = fp->path[i][j];
        }
    }
    output[used] = '\0';
    return true;
}

static bool fs_native_type_matches(FS type, u8 bits) {
    if (type == fat12)
        return bits == FAT_NATIVE_FAT12;
    if (type == fat16)
        return bits == FAT_NATIVE_FAT16;
    if (type == fat32)
        return bits == FAT_NATIVE_FAT32;
    return false;
}

static fat_node_fs *fs_native_filesystem(u8 volume) {
    if (volume >= LETTERS || !volumes || !disktable)
        return 0;
    unsigned int disk = volumes[volume].disk;
    unsigned int partition = volumes[volume].partition;
    if (disk >= 4 || partition >= 4)
        return 0;
    partinfo *part = &disktable[disk].parts[partition];
    if (part->fs != fat12 && part->fs != fat16 && part->fs != fat32)
        return 0;
    fs_native_cache *cache = &native_cache[volume];
    if (cache->valid && cache->disk == disk && cache->partition == partition &&
        cache->location == part->loc && cache->sectors == part->size && cache->type == (u8)part->fs)
        return &cache->filesystem;
    cache->valid = 0;
    if (!fat_node_mount(&cache->filesystem, disk, partition) ||
        !fs_native_type_matches(part->fs, cache->filesystem.volume.fat_bits))
        return 0;
    cache->disk = disk;
    cache->partition = partition;
    cache->location = part->loc;
    cache->sectors = part->size;
    cache->type = (u8)part->fs;
    cache->valid = 1;
    return &cache->filesystem;
}

FILE *fopen(char *path, u8 flags) {
    u8 mode = flags;
    if (mode == 0)
        mode = FOPEN_READ;
    if ((mode & ~(FOPEN_READ | FOPEN_WRITE | FOPEN_CREATE | FOPEN_TRUNCATE | FOPEN_APPEND)) != 0 ||
        !(mode & (FOPEN_READ | FOPEN_WRITE)) ||
        ((mode & (FOPEN_CREATE | FOPEN_TRUNCATE | FOPEN_APPEND)) && !(mode & FOPEN_WRITE)))
        return 0;
    u8 volume;
    const char *relative;
    char **components = 0;
    u32 component_count = 0;
    if (!fs_parse_volume_path(path, &volume, &relative) ||
        !fs_split_path(relative, &components, &component_count))
        return 0;
    u32 slot = 0;
    while (slot < MAX_OPEN_FILES && files[slot].used)
        ++slot;
    if (slot >= MAX_OPEN_FILES) {
        fs_free_path(components, component_count);
        return 0;
    }
    FILE *fp = &files[slot];
    memset(fp, 0, sizeof(*fp));
    fp->used = 1;
    fp->path = components;
    fp->path_count = component_count;
    fp->volume = volume;
    fp->mode = mode;
    char node_path[FILENAME_LENGTH];
    fat_node node;
    fat_node_fs *filesystem = fs_native_filesystem(volume);
    if (!filesystem || !fs_join_file_path(fp, node_path))
        goto fail;
    bool found = fat_node_lookup(filesystem, node_path, &node);
    if (found && (node.attributes & 0x10U))
        goto fail;
    if (!found && (!(mode & FOPEN_WRITE) || (!(mode & FOPEN_CREATE) && !(mode & FOPEN_APPEND))))
        goto fail;
    if (found && (mode & FOPEN_WRITE) && (node.attributes & 0x01U))
        goto fail;
    u32 initial_size = found ? node.size : 0;
    if (mode & FOPEN_TRUNCATE)
        initial_size = 0;
    u8 *write_buffer = 0;
    u32 write_capacity = 0;
    if ((mode & FOPEN_WRITE) && initial_size != 0) {
        write_buffer = malloc(initial_size);
        if (!write_buffer)
            goto fail;
        write_capacity = initial_size;
        if (!(mode & FOPEN_TRUNCATE)) {
            size_t loaded = 0;
            if (!found || !fat_node_read(filesystem, &node, 0, write_buffer, initial_size, &loaded) || loaded != initial_size) {
                free(write_buffer);
                goto fail;
            }
        } else {
            memset(write_buffer, 0, initial_size);
        }
    }
    fp->size = initial_size;
    fp->native_first_cluster = found ? node.first_cluster : 0;
    fp->native_attributes = found ? node.attributes : 0x20U;
    fp->native = 1;
    fp->write_buffer = write_buffer;
    fp->write_capacity = write_capacity;
    fp->write_pending = (mode & FOPEN_WRITE) && (!found || (mode & FOPEN_TRUNCATE));
    fp->pointer = (mode & FOPEN_APPEND) ? initial_size : 0;
    return fp;

fail:
    if (fp->write_buffer)
        free(fp->write_buffer);
    fp->used = 0;
    fp->path = 0;
    fp->path_count = 0;
    fp->write_buffer = 0;
    fs_free_path(components, component_count);
    memset(fp, 0, sizeof(*fp));
    return 0;
}

int aligncheck(FILE *fp) {
    return fs_file_slot_status(fp);
}

int fclose(FILE *fp) {
    if (!fs_file_is_open(fp))
        return 1;
    int result = 0;
    if ((fp->mode & FOPEN_WRITE) && fp->write_pending) {
        char node_path[FILENAME_LENGTH];
        fat_node_fs *filesystem = fs_native_filesystem(fp->volume);
        if (!filesystem || !fs_join_file_path(fp, node_path) || fp->size > 0xFFFFFFFFULL) {
            result = -1;
        } else {
            int write_result = fat_node_write_file(filesystem, node_path, fp->write_buffer, (u32)fp->size);
            if (write_result != FAT_WRITE_OK)
                result = -1;
        }
    }
    if (fp->write_buffer)
        free(fp->write_buffer);
    fs_free_path(fp->path, fp->path_count);
    memset(fp, 0, sizeof(*fp));
    return result;
}

size_t fread(void *ptr, size_t bytes, FILE *fp) {
    if (!ptr || !fs_file_is_open(fp) || !(fp->mode & FOPEN_READ) || bytes == 0 || fp->pointer >= fp->size)
        return 0;
    if ((u64)bytes > fp->size - fp->pointer)
        bytes = (size_t)(fp->size - fp->pointer);
    if (fp->mode & FOPEN_WRITE) {
        if (!fp->write_buffer)
            return 0;
        memcpy(ptr, fp->write_buffer + fp->pointer, bytes);
        fp->pointer += bytes;
        return bytes;
    }
    fat_node_fs *filesystem = fs_native_filesystem(fp->volume);
    if (!filesystem)
        return 0;
    fat_node node;
    node.first_cluster = fp->native_first_cluster;
    node.size = (u32)fp->size;
    node.attributes = fp->native_attributes;
    node.is_root = 0;
    node.fixed_root = 0;
    node.reserved = 0;
    size_t read_count = 0;
    if (!fat_node_read(filesystem, &node, fp->pointer, ptr, bytes, &read_count))
        return 0;
    fp->pointer += read_count;
    return read_count;
}

size_t fwrite(const void *ptr, size_t bytes, FILE *fp) {
    if (!ptr || !fs_file_is_open(fp) || !(fp->mode & FOPEN_WRITE) || bytes == 0)
        return 0;
    if (fp->mode & FOPEN_APPEND)
        fp->pointer = (size_t)fp->size;
    u64 end = (u64)fp->pointer + (u64)bytes;
    if (end > 0xFFFFFFFFULL)
        return 0;
    u32 required = (u32)end;
    if (required > fp->write_capacity) {
        u32 capacity = fp->write_capacity ? fp->write_capacity : 64U;
        while (capacity < required) {
            if (capacity > 0x7FFFFFFFU) {
                capacity = required;
                break;
            }
            capacity *= 2U;
        }
        u8 *replacement = malloc(capacity);
        if (!replacement)
            return 0;
        memset(replacement, 0, capacity);
        if (fp->size != 0 && fp->write_buffer)
            memcpy(replacement, fp->write_buffer, (size_t)fp->size);
        if (fp->write_buffer)
            free(fp->write_buffer);
        fp->write_buffer = replacement;
        fp->write_capacity = capacity;
    }
    if (fp->pointer > fp->size)
        memset(fp->write_buffer + fp->size, 0, (size_t)(fp->pointer - fp->size));
    memcpy(fp->write_buffer + fp->pointer, ptr, bytes);
    fp->pointer = (size_t)end;
    if (end > fp->size)
        fp->size = end;
    fp->write_pending = 1;
    return bytes;
}

int fseek(FILE *fp, long long offset, fpos_t position) {
    if (!fs_file_is_open(fp))
        return -1;
    bool writable = (fp->mode & FOPEN_WRITE) != 0;
    u64 limit = writable ? 0xFFFFFFFFULL : fp->size;
    if (fp->pointer > fp->size && !writable)
        return -2;
    switch (position) {
        case SEEK_SET:
            if (offset < 0 || (u64)offset > limit)
                return -2;
            fp->pointer = (u64)offset;
            break;
        case SEEK_CUR:
            if (offset < 0) {
                u64 distance = (u64)(-(offset + 1)) + 1U;
                if (distance > fp->pointer)
                    return -2;
                fp->pointer -= distance;
            } else {
                u64 distance = (u64)offset;
                if (distance > limit - fp->pointer)
                    return -2;
                fp->pointer += distance;
            }
            break;
        case SEEK_END:
            if (offset < 0) {
                u64 distance = (u64)(-(offset + 1)) + 1U;
                if (distance > fp->size)
                    return -2;
                fp->pointer = fp->size - distance;
            } else {
                u64 distance = (u64)offset;
                if (distance > limit - fp->size)
                    return -2;
                fp->pointer = fp->size + distance;
            }
            break;
        default:
            return -1;
    }
    return 0;
}

void rewind(FILE *fp) {
    if (fs_file_is_open(fp))
        fp->pointer = 0;
}

unsigned long long fsize(FILE *fp) {
    return fs_file_is_open(fp) ? fp->size : 0;
}

DIR *opendir(char *path) {
    u8 volume;
    const char *relative;
    if (!fs_parse_volume_path(path, &volume, &relative))
        return 0;
    fat_node_fs *filesystem = fs_native_filesystem(volume);
    if (!filesystem)
        return 0;
    fat_node node;
    if (!fat_node_lookup(filesystem, relative, &node) || !(node.attributes & 0x10U))
        return 0;
    for (u32 i = 0; i < MAX_OPEN_DIRS; ++i) {
        if (dir_table[i].used)
            continue;
        fat_node_dir *iterator = malloc(sizeof(fat_node_dir));
        if (!iterator)
            return 0;
        if (!fat_node_dir_open(filesystem, &node, iterator)) {
            free(iterator);
            return 0;
        }
        DIR *dir = &dir_table[i];
        memset(dir, 0, sizeof(*dir));
        dir->used = 1;
        dir->volume = volume;
        dir->fs = (u8)disktable[volumes[volume].disk].parts[volumes[volume].partition].fs;
        dir->reserved = 1;
        dir->fsdata[0] = (u64)(uintptr_t)iterator;
        return dir;
    }
    return 0;
}

dirent *readdir(DIR *dir) {
    if (!fs_dir_is_open(dir))
        return 0;
    if (dir->reserved == 1) {
        fat_node_dir *iterator = (fat_node_dir *)(uintptr_t)dir->fsdata[0];
        if (!iterator)
            return 0;
        fat_node_entry entry;
        int result = fat_node_dir_next(iterator, &entry);
        if (result != FAT_NODE_ENTRY)
            return 0;
        memset(dir->entry.name, 0, sizeof(dir->entry.name));
        for (u32 i = 0; i < sizeof(dir->entry.name) - 1 && entry.name[i]; ++i)
            dir->entry.name[i] = entry.name[i];
        dir->entry.type = entry.type == FAT_NODE_DIRECTORY ? DIRENT_DIR : DIRENT_FILE;
        dir->entry.size = entry.node.size;
        return &dir->entry;
    }
    switch (dir->fs) {
        case fat16:
            if (fat16_dir_read(dir))
                return &dir->entry;
            return 0;
        case fat32:
            if (fat32_dir_read(dir))
                return &dir->entry;
            return 0;
        default:
            return 0;
    }
}

int closedir(DIR *dir) {
    if (!fs_dir_is_open(dir))
        return 1;
    if (dir->reserved == 1) {
        fat_node_dir *iterator = (fat_node_dir *)(uintptr_t)dir->fsdata[0];
        if (iterator)
            free(iterator);
    } else if (dir->fs == fat32) {
        fat32_dir_close(dir);
    }
    memset(dir, 0, sizeof(*dir));
    return 0;
}
