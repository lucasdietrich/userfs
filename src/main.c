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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include <cstdio>
#include <errno.h>

#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "userfs.h"
#include "btrfs.h"
#include "utils.h"
#include "overlays.h"
#include "disk.h"

int verbose = 0;

static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Manage userfs partition on %s\n\n", DISK);
    printf("Options:\n");
    printf("  -d    Delete partition %u (userfs) if it exists\n", USERFS_PART_NO);
    printf("  -t    Trust existing userfs filesystem (if valid) after partition creation "
           "(first boot)\n");
    printf("  -f	Force mkfs.btrfs even if already initialized (mutually exclusive "
           "with -t)\n");
    printf("  -o    Skip overlayfs setup (useful for debugging)\n");
    printf("  -v    Enable verbose output\n");
    printf("  -h    Show this help message\n");
    printf("  (no args) Create partition %u (userfs) if it doesn't exist\n",
           USERFS_PART_NO);
    printf("\n");
}

static int parse_args(int argc, char *argv[], struct args *args)
{
    int opt;

    if (!args) {
        fprintf(stderr, "Invalid arguments\n");
        return -1;
    }

    while ((opt = getopt(argc, argv, "hdfvot")) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        case 'd':
            args->flags |= FLAG_USERFS_DELETE;
            break;
        case 'f':
            args->flags |= FLAG_USERFS_FORCE_FORMAT;
            break;
        case 't':
            args->flags |= FLAG_USERFS_TRUST_RESIDENT;
            break;
        case 'o':
            args->flags |= FLAG_USERFS_SKIP_OVERLAYS;
            break;
        case 'v':
            verbose = 1;
            break;
        case '?':
            fprintf(stderr, "Unknown option: -%c\n", opt);
            print_usage(argv[0]);
            return -1;
        default:
            fprintf(stderr, "Unknown option: -%c\n", opt);
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int ret               = -1;
    struct disk_info disk = {0};
    struct args args      = {0};

    ret = parse_args(argc, argv, &args);
    if (ret != 0) {
        fprintf(stderr, "Failed to parse arguments\n");
        goto exit;
    }

    // STEP1: Inspect the disk and create userfs partition if it doesn't exist
    ret = step1_create_userfs_partition(&args, &disk);
    if (ret != 0) {
        fprintf(stderr, "Failed to create userfs partition: %s\n", strerror(errno));
        goto exit;
    }

    struct part_info *userfs_part = &disk.partitions[USERFS_PART_NO];

    // Some assertions ...
    ASSERT(userfs_part->used, "Userfs partition should be created and in use");
    ASSERT(userfs_part->partno == USERFS_PART_NO,
           "Userfs partition number should match expected value");

    // partprob
    ret = disk_partprobe(DISK);
    if (ret < 0) {
        fprintf(stderr, "Failed to partprobe: %s\n", strerror(errno));
        goto exit;
    }

    // STEP2: Create BTRFS filesystem on the userfs partition
    ret = step2_create_btrfs_filesystem(&args, userfs_part);
    if (ret != 0) {
        fprintf(stderr, "Failed to create BTRFS filesystem: %s\n", strerror(errno));
        goto exit;
    }

    if (args.flags & FLAG_USERFS_SKIP_OVERLAYS) {
        printf("Skipping overlayfs setup as per user request\n");
        disk_clear_info(&disk);
        return 0; // Nothing more to do
    }

    // STEP3: Create overlayfs for /etc, /var and /home
    ret = step3_create_overlayfs(&args);
    if (ret != 0) {
        fprintf(stderr, "Failed to create overlayfs: %s\n", strerror(errno));
        goto exit;
    }

    disk_clear_info(&disk);
    return 0;

exit:
    disk_clear_info(&disk);
    return ret;
}
