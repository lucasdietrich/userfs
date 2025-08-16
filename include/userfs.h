#ifndef USERFS_H
#define USERFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "disk.h"

#ifndef DISK
#define DISK "/dev/mmcblk0"
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

int step2_create_btrfs_filesystem(struct args *args, struct part_info *userfs_part);

int step3_create_overlayfs(struct args *args);


#endif /* USERFS_H */