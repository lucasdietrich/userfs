/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
 
#ifndef USERFS_DISK_H
#define USERFS_DISK_H

#include <stddef.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libfdisk/libfdisk.h>
#include <linux/limits.h>

#define MAX_DOS_PARTITIONS       4u
#ifndef MAX_SUPPORTED_PARTITIONS
#define MAX_SUPPORTED_PARTITIONS 16u
#endif

// TODO move to another header
#define USERFS_PART_LABEL "userfs"
#define TEEFS_PART_LABEL "tee-fs"
#define SWAP_PART_LABEL "swap"
#define MANUFACTURER_PART_LABEL "manufacturer-data"

enum fs_type {
    FS_TYPE_UNKNOWN   = 0,
    FS_TYPE_BTRFS     = 1,
    FS_TYPE_EXT4      = 2,
    FS_TYPE_SWAP      = 3,
    FS_TYPE_VFAT      = 4,
    FS_TYPE_INTEGRITY = 5,
    FS_TYPE_LVM       = 6,
};

struct fs_info {
    enum fs_type type;
    char uuid[37u]; // UUID is 36 characters + null terminator
    char part_label[64u];
};

struct part_info {
    /* fdisk infos */
    fdisk_sector_t start;
    fdisk_sector_t end;
    fdisk_sector_t size;
    size_t partno;
    int used;
    int type; // type code, Linux, Swap, Extended, FAT32, ...
    const char *type_name;
    const char *part_label;

    /* absolute path to partition device */
    char path[PATH_MAX];

    /* FS informations if any */
    bool fs_probed;
    struct fs_info fs_info;
};

struct disk_info {
    int type;
    fdisk_sector_t nsectors;
    uint64_t total_size; // in bytes

    size_t partition_count;

    struct part_info partitions[MAX_SUPPORTED_PARTITIONS];
    size_t last_used_partno;

    size_t next_free_sector;
    size_t free_sectors;
    uint64_t free_size; // in bytes
};

struct block_device {
    /* infos */
    uint64_t sectors;

    /* absolute path to partition device */
    char path[PATH_MAX];
};

struct mapper {
    /* infos */
    uint64_t sectors;

    /* absolute path to partition device */
    char path[PATH_MAX];
};

int disk_partprobe(const char *device);

void disk_clear_info(struct disk_info *disk);

struct part_info *disk_find_partition_by_label(struct disk_info *disk,
                                               const char *partlabel);

void mapper_info_display(const struct block_device *mapper);

#endif /* USERFS_DISK_H */