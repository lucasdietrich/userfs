/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
 
#include "userfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <blkid.h>
#include <linux/fs.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

static const char *btrfs_subvolumes[] = {
    [BTRFS_SV_DATA_INDEX]   = "vol-data",
    [BTRFS_SV_CONFIG_INDEX] = "vol-config",
};

const char *btrfs_get_volume(size_t sv_index)
{
    if (sv_index >= ARRAY_SIZE(btrfs_subvolumes)) {
        return NULL;
    }
    return btrfs_subvolumes[sv_index];
}

int step2_create_btrfs_filesystem(struct args *args, struct disk_info *disk, size_t userfs_partno)
{
    int ret = -1;

    struct part_info *userfs_part = &disk->partitions[userfs_partno];

    // Some assertions ...
    ASSERT(userfs_part->used, "Userfs partition should be created and in use");
    ASSERT(userfs_part->partno == userfs_partno,
           "Userfs partition number should match expected value");

    // Some assertions ...
    ASSERT(userfs_part->used, "Userfs partition should be created and in use");
    ASSERT(userfs_part->partno == USERFS_PART_NO,
           "Userfs partition number should match expected value");

    // inspect the partition info after changes
    char userfs_part_device[PATH_MAX];
    ret = disk_part_build_path(
        userfs_part_device, sizeof(userfs_part_device), userfs_part->partno);
    if (ret < 0) {
        fprintf(stderr, "Failed to build userfs partition path: %s\n", strerror(errno));
        goto exit;
    }

    LOG("Userfs partition device: %s\n", userfs_part_device);

    ret = fs_probe(userfs_part_device, &userfs_part->fs_info);
    if (ret != 0) {
        fprintf(stderr,
                "Failed to probe filesystem on %s: %s\n",
                userfs_part_device,
                strerror(errno));
        goto exit;
    }

    fs_info_display(&userfs_part->fs_info);

    bool do_format_btrfs = false;
    if (args->flags & FLAG_USERFS_FORCE_FORMAT) {
        do_format_btrfs = true;
        LOG("Userfs partition (%s) will be formatted to BTRFS due to force flag\n",
            userfs_part_device);
    }

    switch (userfs_part->fs_info.type) {
    case FS_TYPE_BTRFS:
        printf("Userfs partition %zu already formatted as BTRFS, skipping\n",
               userfs_part->partno);
        break;
    case FS_TYPE_EXT4:
        printf("Userfs partition %zu already formatted as EXT4, skipping\n",
               userfs_part->partno);
        break;
    case FS_TYPE_UNKNOWN:
    default:
        do_format_btrfs = true;
        break;
    }

    if (do_format_btrfs) {
        // If the userfs partition is not BTRFS, create it
        LOG("Creating BTRFS filesystem on %s\n", userfs_part_device);

        const char *const mkfs_args[] = {
            "mkfs.btrfs",
            "-f", // Force creation
            userfs_part_device,
            NULL,
        };

        command_display(mkfs_args[0], (char *const *)mkfs_args);
        ret = command_run(NULL, NULL, mkfs_args[0], (char *const *)mkfs_args);
        LOG("mkfs.btrfs returned: %d\n", ret);
        if (ret < 0) {
            fprintf(stderr, "Failed to create BTRFS filesystem: %s\n", strerror(errno));
            goto exit;
        }

        LOG("BTRFS filesystem created successfully on %s\n", userfs_part_device);

        // Create the mount point if it doesn't exist
        ret = create_directory(USERFS_MOUNT_POINT);
        if (ret != 0) {
            fprintf(stderr,
                    "Failed to create mount point %s: %s\n",
                    USERFS_MOUNT_POINT,
                    strerror(errno));
            goto exit;
        }
    }

    // Mount the btrfs filesystem
    LOG("Mounting BTRFS filesystem on %s\n", USERFS_MOUNT_POINT);

    ret = mount(userfs_part_device, USERFS_MOUNT_POINT, "btrfs", 0, NULL);
    if (ret != 0) {
        fprintf(stderr,
                "Failed to mount BTRFS filesystem on %s: %s\n",
                USERFS_MOUNT_POINT,
                strerror(errno));
        goto exit;
    }

    if (do_format_btrfs) {
        // Create subvolumes
        // FIXME use the btrfs library instead of running commands

        char sv_name[PATH_MAX];
        const char *btrfs_create_subvolumes[] = {
            "btrfs",
            "subvolume",
            "create",
            sv_name, // Placeholder for subvolume name
            NULL,    // End of arguments
        };

        for (size_t sv = 0u; sv < ARRAY_SIZE(btrfs_subvolumes); sv++) {
            snprintf(sv_name,
                     sizeof(sv_name),
                     "%s/%s",
                     USERFS_MOUNT_POINT,
                     btrfs_subvolumes[sv]);

            LOG("Creating BTRFS subvolume: %s\n", sv_name);

            command_display(btrfs_create_subvolumes[0],
                            (char *const *)btrfs_create_subvolumes);
            ret = command_run(NULL,
                              NULL,
                              btrfs_create_subvolumes[0],
                              (char *const *)btrfs_create_subvolumes);
            if (ret < 0) {
                fprintf(stderr,
                        "Failed to create BTRFS subvolume %s: %s\n",
                        btrfs_create_subvolumes[3],
                        strerror(errno));
                goto exit;
            }
        }
    }

    return 0;

exit:
    return ret;
}
