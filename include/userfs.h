/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// clang-format off
 /*
 * Userfs Partition Management Tool
 *
 * This program manages a userfs partition on /dev/mmcblk0 and sets up
 * persistent storage with overlayfs for an embedded Linux system.
 *
 * MAIN COMMAND FLOW:
 *
 * 1. PARSE ARGUMENTS:
 *    - Parse command line options:
 *      * -v: Enable verbose output
 *      * -d: Delete userfs partition and exit
 *      * -f: Force mkfs.btrfs even if already initialized (mutually exclusive with -t)
 *      * -t: Trust existing userfs filesystem after partition creation (first boot only)
 *      * -o: Skip overlayfs setup (useful for debugging)
 *      * -h: Show help message
 *
 * 2. DISK INSPECTION & PARTITION MANAGEMENT:
 *    - Get disk size using ioctl(BLKGETSIZE64) on /dev/mmcblk0
 *    - Read existing partition table using libfdisk
 *    - Analyze current partitions (boot, rootfs, userfs)
 *
 *    IF delete flag (-d):
 *      - Delete userfs partition (partition #2) and exit
 *    ELSE:
 *      - Create userfs partition if it doesn't exist using remaining free space
 *      - Write partition table changes to disk using fdisk_write_disklabel()
 *      
 *    FIRST BOOT vs SUBSEQUENT BOOT LOGIC:
 *      - If partition was just created (first boot):
 *        * Default: Force format to BTRFS (ignores existing filesystem)
 *        * With -t flag: Trust existing filesystem without formatting
 *      - If partition already existed (subsequent boots):
 *        * Preserve existing filesystem unless -f flag is used
 *
 * 3. PARTITION TABLE REFRESH:
 *    - Run `partprobe /dev/mmcblk0` to refresh kernel partition table
 *    - Wait for device nodes to appear
 *
 * 4. FILESYSTEM PROBING:
 *    - Probe filesystem on userfs partition (/dev/mmcblk0p3) using libblkid
 *    - Detect existing filesystem type and UUID
 *
 * 5. BTRFS FILESYSTEM CREATION:
 *    - Skip if already BTRFS and not forced (-f flag) and not first boot
 *    - Run `mkfs.btrfs -f /dev/mmcblk0p3` if:
 *      * Partition is unformatted, OR
 *      * Force flag (-f) is used, OR  
 *      * First boot and trust flag (-t) is NOT used
 *    - Create mount point /mnt/userfs
 *    - Mount BTRFS filesystem on /mnt/userfs
 *    - Create BTRFS subvolumes (only when filesystem is newly created):
 *      * vol-data (for /var and /home overlays)
 *      * vol-config (for /etc overlay)
 *
 * 6. OVERLAYFS SETUP (skipped if -o flag used):
 *    - Unmount existing /var/volatile tmpfs
 *    - For each mount point (/etc, /var, /home):
 *      * Create upper and work directories in appropriate BTRFS subvolumes
 *      * Unmount existing mount if present
 *      * Mount overlayfs with lowerdir=original, upperdir=persistent, workdir=work
 *    - Remount /var/volatile as tmpfs with mode 0755
 *
 * RESULT:
 * The program creates a persistent userfs partition with BTRFS and sets up
 * overlayfs mounts to make /etc, /var, and /home writable and persistent
 * on what appears to be an embedded Linux system with read-only root filesystem.
 *
 * This scripts can be illustrated as follows:
 * 
 *     # create a userfs partition at /dev/mmcblk0p3 using fdisk
 *  
 *     partprobe /dev/mmcblk0
 *     /usr/bin/mkfs.btrfs -f /dev/mmcblk0p3
 *  
 *     # mount userfs partition
 *     mkdir -p /mnt/userfs
 *     mount -t btrfs /dev/mmcblk0p3 /mnt/userfs
 *  
 *     # create subvolumes
 *     if ! btrfs subvolume show /mnt/userfs/vol-data >/dev/null 2>&1; then
 *        btrfs subvolume create /mnt/userfs/vol-data
 *     fi
 *     if ! btrfs subvolume show /mnt/userfs/vol-config >/dev/null 2>&1; then
 *        btrfs subvolume create /mnt/userfs/vol-config
 *     fi
 *  
 *     # if /var/volatile is already mounted, we need to unmount it first
 *     if mount | grep /var/volatile > /dev/null; then
 *        umount /var/volatile
 *     fi
 *  
 *     # create overlayfs for var, etc, and home
 *     mkdir -p /mnt/userfs/vol-config/etc /mnt/userfs/vol-config/.work.etc
 *     mount -t overlay overlay \
 *        -o lowerdir=/etc,upperdir=/mnt/userfs/vol-config/etc,workdir=/mnt/userfs/vol-config/.work.etc \
 *        /etc
 *  
 *     mkdir -p /mnt/userfs/vol-data/var /mnt/userfs/vol-data/.work.var
 *     mount -t overlay overlay \
 *        -o lowerdir=/var,upperdir=/mnt/userfs/vol-data/var,workdir=/mnt/userfs/vol-data/.work.var \
 *        /var
 *  
 *     mkdir -p /mnt/userfs/vol-data/home /mnt/userfs/vol-data/.work.home
 *     mount -t overlay overlay \
 *        -o lowerdir=/home,upperdir=/mnt/userfs/vol-data/home,workdir=/mnt/userfs/vol-data/.work.home \
 *        /home
 *  
 *     # mount /var/volatile again
 *     mount -t tmpfs tmpfs /var/volatile
 */
// clang-format on

#ifndef USERFS_H
#define USERFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "disk.h"
#include "btrfs.h"
#include "utils.h"
#include "fs.h"

#ifndef DISK
#define DISK "/dev/mmcblk0"
#endif

#if defined(USERFS_BLOCK_DEVICE_TYPE_MMC)
#define DISK_PART_FMT "%sp%zu"
#elif defined(USERFS_BLOCK_DEVICE_TYPE_DISK)
#define DISK_PART_FMT "%s%zu"
#else
#error "Unsupported block device type"
#endif

#define USERFS_MOUNT_POINT "/mnt/userfs"

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

int step1_create_userfs_partition(struct args *args, struct disk_info *disk);

int step2_create_btrfs_filesystem(struct args *args, struct disk_info *disk, size_t userfs_partno);

int step3_create_overlayfs(struct args *args);

int step4_format_swap_partition(struct args *args, struct disk_info *disk, size_t swap_partno);

#endif /* USERFS_H */