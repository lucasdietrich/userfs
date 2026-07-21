/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include <cstdio>
#include "disk.h"
#include "dm.h"
#include "ext4.h"
#include "manufacturer-partitions.h"
#include "teefs.h"
#include "userfs.h"
#include "utils.h"

#include <errno.h>

#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int verbose = 0;

static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Manage userfs partition on %s by default\n\n", DEFAULT_DISK);
    printf("Options:\n");
    printf("  -b <device>  Override the block device path at runtime\n");
    printf("  -d    Delete userfs partition if it exists\n");
    printf("  -t    Trust existing userfs filesystem (if valid) after partition creation "
           "(first boot)\n");
    printf("  -f	Force mkfs.btrfs even if already initialized (mutually exclusive "
           "with -t)\n");
    printf("  -o    Skip overlayfs setup (useful for debugging)\n");
    printf("  -v    Enable verbose output\n");
    printf("  -u    Undo all changes made by this program (delete userfs partition, "
           "remove overlays, etc.)\n");
    printf("  -h    Show this help message\n");
    printf("  (no args) Create (userfs) partition if it doesn't exist\n");
    printf("\n");
}

static int parse_args(int argc, char *argv[], struct args *args)
{
    int opt;

    if (!args) {
        ERR("Invalid arguments\n");
        return -1;
    }

    while ((opt = getopt(argc, argv, "hb:dfvotu")) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        case 'b':
            args->dev_base = optarg;
            break;
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
        case 'u':
            args->flags |= FLAG_UNDO_ALL;
            break;
        case '?':
            ERR("Unknown option: -%c\n", opt);
            print_usage(argv[0]);
            return -1;
        default:
            ERR("Unknown option: -%c\n", opt);
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
    struct args args      = {
             .dev_base = DEFAULT_DISK,
    };

    ret = parse_args(argc, argv, &args);
    if (ret != 0) {
        ERR("Failed to parse arguments\n");
        goto exit;
    }

    // partprob
    ret = disk_partprobe(args.dev_base);
    if (ret < 0) {
        ERR("Failed to partprobe: %s\n", strerror(errno));
        goto exit;
    }

    // STEP1: Inspect the disk and create userfs partition if it doesn't exist
    ret = setup_userfs(&args, &disk);
    if (ret != 0) {
        ERR("Failed to create userfs partition: %s\n", strerror(errno));
        goto exit;
    }

    bool force;

#if FEATURE_TEEFS
    // Create TEEs partition if not already present
    struct part_info *teefs_part = disk_find_partition_by_label(&disk, TEEFS_PART_LABEL);
    if (!teefs_part) {
        ERR("Failed to find TEEs partition\n");
        goto exit;
    }

    struct block_device teefs_mapper = {0};
    force                            = (args.flags & FLAG_USERFS_FORCE_FORMAT) != 0;
    ret                              = setup_teefs(teefs_part, force, &teefs_mapper);
    if (ret != 0) {
        ERR("Failed to create TEEs partition: %s\n", strerror(errno));
        goto exit;
    }

    ret = create_directory(TEEFS_MOUNT_POINT);
    if (ret != 0) {
        ERR("Failed to create TEEs mount point: %s\n", strerror(errno));
        goto exit;
    }

    LOG("[ mount %s -> %s ] fstype: ext4, flags: noatime,nodev,nosuid,noexec,nosymfollow "
        "with options: errors=remount-ro\n",
        teefs_mapper.path,
        TEEFS_MOUNT_POINT);
    ret = mount(teefs_mapper.path,
                TEEFS_MOUNT_POINT,
                "ext4",
                MS_NOATIME | MS_NODEV | MS_NOSUID | MS_NOEXEC | MS_NOSYMFOLLOW,
                "errors=remount-ro");
    if (ret != 0) {
        ERR("Failed to mount TEEs partition: %s\n", strerror(errno));
        goto exit;
    }

    if (args.flags & FLAG_UNDO_ALL) {
        ret = umount(TEEFS_MOUNT_POINT);
        if (ret != 0)
            LOG("Failed to unmount TEEs partition: %s\n", strerror(errno));

        ret = remove(TEEFS_MOUNT_POINT);
        if (ret != 0)
            LOG("Failed to remove TEEs mount point: %s\n", strerror(errno));

        ret = clear_teefs(teefs_part, true);
        if (ret != 0)
            LOG("Failed to clear TEEs partition: %s\n", strerror(errno));
    }
#endif /* FEATURE_TEEFS */

#if FEATURE_MANUFACTURER_PARTITION
    // Create manufacturer partition if not already present
    struct part_info *manufacturer_part =
        disk_find_partition_by_label(&disk, MANUFACTURER_PART_LABEL);
    if (!manufacturer_part) {
        ERR("Failed to find manufacturer partition\n");
        goto exit;
    }

    struct block_device manuf_mapper = {0};
    force                            = (args.flags & FLAG_USERFS_FORCE_FORMAT) != 0;
    ret = setup_manufacturer_data(manufacturer_part, force, &manuf_mapper);
    if (ret != 0) {
        ERR("Failed to create manufacturer partition: %s\n", strerror(errno));
        goto exit;
    }

    LOG("[ symlink %s -> %s ]\n", manuf_mapper.path, MANUFACTURER_MOUNT_POINT);
    ret = symlink(manuf_mapper.path, MANUFACTURER_MOUNT_POINT);
    if (ret != 0) {
        ERR("Failed to create symlink for manufacturer partition: %s\n", strerror(errno));
        goto exit;
    }

    if (args.flags & FLAG_UNDO_ALL) {
        ret = remove(MANUFACTURER_MOUNT_POINT);
        if (ret != 0)
            LOG("Failed to remove manufacturer symlink: %s\n", strerror(errno));

        ret = clear_manufacturer_data(manufacturer_part, true);
        if (ret != 0)
            LOG("Failed to clear manufacturer partition: %s\n", strerror(errno));
    }
#endif /* FEATURE_MANUFACTURER_PARTITION */

    // STEP2: Create BTRFS filesystem on the userfs partition
    struct part_info *userfs_part;
#if USERFS_PARTITION_TABLE_DOS
    userfs_part = &disk.partitions[USERFS_PART_NO];
#elif USERFS_PARTITION_TABLE_GPT
    userfs_part = disk_find_partition_by_label(&disk, USERFS_PART_LABEL);
#endif
    if (!userfs_part) {
        ERR("Userfs partition not found after creation\n");
        goto exit;
    }

    ret = create_btrfs_filesystem(&args, userfs_part);
    if (ret != 0) {
        ERR("Failed to create BTRFS filesystem: %s\n", strerror(errno));
        goto exit;
    }

    if ((args.flags & FLAG_USERFS_SKIP_OVERLAYS) == 0) {
        // STEP3: Create overlayfs for /etc, /var and /home
        ret = setup_overlayfs();
        if (ret != 0) {
            ERR("Failed to create overlayfs: %s\n", strerror(errno));
            goto exit;
        }
    } else {
        printf("Skipping overlayfs setup as per user request\n");
    }

// Create TEEs partition if not already present
#if defined(SWAP_PART)
    struct part_info *swap_part = disk_find_partition_by_label(&disk, SWAP_PART_LABEL);
    if (!swap_part) {
        ERR("Failed to find swap partition\n");
        goto exit;
    }

    ret = format_swap_partition(swap_part);
    if (ret != 0) {
        ERR("Failed to format swap partition: %s\n", strerror(errno));
        goto exit;
    }
#endif /* SWAP_PART */

exit:
    disk_clear_info(&disk);
    return ret;
}
