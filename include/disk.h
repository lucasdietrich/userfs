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

#define MAX_DOS_PARTITIONS       4u
#define MAX_SUPPORTED_PARTITIONS 6u

enum fs_type {
    FS_TYPE_UNKNOWN = 0,
    FS_TYPE_BTRFS   = 1,
    FS_TYPE_EXT4    = 2,
    FS_TYPE_SWAP    = 3,
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

int disk_partprobe(const char *device);

void disk_clear_info(struct disk_info *disk);

ssize_t disk_part_build_path(char *buf, size_t buf_len, size_t partno);

#endif /* USERFS_DISK_H */