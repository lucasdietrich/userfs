#ifndef USERFS_H
#define USERFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "libfdisk/libfdisk.h"

#ifndef DISK
#define DISK "/dev/mmcblk0"
#endif

#define USERFS_MOUNT_POINT "/mnt/userfs"

#define MAX_DOS_PARTITIONS       4u
#define MAX_SUPPORTED_PARTITIONS 6u

#define BOOT_PART_NO   0u
#define ROOTFS_PART_NO 1u
#ifndef USERFS_PART_NO
#define USERFS_PART_NO 2u
#endif /* USERFS_PART_NO */

#define FLAG_USERFS_DELETE         (1 << 1u)
#define FLAG_USERFS_FORCE_FORMAT   (1 << 2u)
#define FLAG_USERFS_TRUST_RESIDENT (1 << 3u)
#define FLAG_USERFS_SKIP_OVERLAYS  (1 << 4u)

extern int verbose;

struct args {
    uint32_t flags; // Bitmask for flags
};

#define LOG(fmt, ...)                                                                    \
    do {                                                                                 \
        if (verbose) {                                                                   \
            printf(fmt, ##__VA_ARGS__);                                                  \
        }                                                                                \
    } while (0)

#define ASSERT(cond, msg)                                                                \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "Assertion failed: %s\n", msg);                              \
            return -1;                                                                   \
        }                                                                                \
    } while (0)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))


enum fs_type {
    FS_TYPE_UNKNOWN = 0,
    FS_TYPE_BTRFS   = 1,
    FS_TYPE_EXT4    = 2,
};

struct fs_info {
    enum fs_type type;
    char uuid[37u]; // UUID is 36 characters + null terminator
    char _reversed[3];
};


struct part_info {
    fdisk_sector_t start;
    fdisk_sector_t end;
    fdisk_sector_t size;
    size_t partno;
    int used;
    int type; // type code, Linux, Swap, Extended, FAT32, ...
    const char *type_name;

    /* FS informations if any */
    struct fs_info fs_info;
};

struct disk_info {
    int type;
    fdisk_sector_t total_sectors;
    uint64_t total_size; // in bytes

    size_t partition_count;

    struct part_info partitions[MAX_SUPPORTED_PARTITIONS];
    size_t last_used_partno;

    size_t next_free_sector;
    size_t free_sectors;
    uint64_t free_size; // in bytes
};

#endif /* USERFS_H */