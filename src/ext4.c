/*
 * Copyright (c) 2026 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "disk.h"
#include "ext4.h"
#include "fs.h"
#include "userfs.h"

int format_ext4(const char *device, bool force)
{
    struct fs_info fs_info;
    int ret = fs_probe(device, &fs_info);
    if (ret == 0 && fs_info.type == FS_TYPE_EXT4 && !force) {
        LOG("[ ext4 %s ] already formatted, skipping mkfs.ext4\n", device);
        return -0;
    }

    const char *mkfs_args[] = {
        "/sbin/mkfs.ext4",
        "-F", // Force creation
        device,
        NULL,
    };

    ret = command_run(NULL, NULL, mkfs_args[0], mkfs_args);
    if (ret < 0) {
        ERR("Failed to create EXT4 filesystem: %s\n", strerror(errno));
        return ret;
    }

    return 0;
}