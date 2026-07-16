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
#include "userfs.h"

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

    while ((opt = getopt(argc, argv, "hb:dfvot")) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        case 'b':
            args->dev = optarg;
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
        .dev = DEFAULT_DISK,
    };

    ret = parse_args(argc, argv, &args);
    if (ret != 0) {
        ERR("Failed to parse arguments\n");
        goto exit;
    }

    // partprob
    ret = disk_partprobe(args.dev);
    if (ret < 0) {
        ERR("Failed to partprobe: %s\n", strerror(errno));
        goto exit;
    }

    // STEP1: Inspect the disk and create userfs partition if it doesn't exist
    ret = step1_create_userfs_partition(&args, &disk);
    if (ret != 0) {
        ERR("Failed to create userfs partition: %s\n", strerror(errno));
        goto exit;
    }

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

    ret = step2_create_btrfs_filesystem(&args, userfs_part);
    if (ret != 0) {
        ERR("Failed to create BTRFS filesystem: %s\n", strerror(errno));
        goto exit;
    }

    if ((args.flags & FLAG_USERFS_SKIP_OVERLAYS) == 0) {
        // STEP3: Create overlayfs for /etc, /var and /home
        ret = step3_create_overlayfs(&args);
        if (ret != 0) {
            ERR("Failed to create overlayfs: %s\n", strerror(errno));
            goto exit;
        }
    } else {
        printf("Skipping overlayfs setup as per user request\n");
    }

#if defined(SWAP_PART_NO)
    // STEP4: Format swap partition if not already formatted
    ret = step4_format_swap_partition(&args, &disk, SWAP_PART_NO);
    if (ret != 0) {
        ERR("Failed to format swap partition: %s\n", strerror(errno));
        goto exit;
    }
#endif /* SWAP_PART_NO */

exit:
    disk_clear_info(&disk);
    return ret;
}
