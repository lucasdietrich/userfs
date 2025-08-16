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
