/*
 * Copyright (c) 2026 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USERFS_DM_H
#define USERFS_DM_H

#include "disk.h"

/**
 * Wait for all pending udev events to be processed.
 * Must be called before dm_remove() if a filesystem was recently written
 * to the mapped device (e.g. after mkfs), otherwise dm_remove() may
 * fail with EBUSY while udev's blkid probe still holds the device open.
 */
int udev_settle(void);

int dm_integrity_map(const char *mapper_name,
                     const char *device,
                     struct block_device *dev,
                     bool reset_integrity_sb);

int dmsetup_create(const char *mapper_name, uint64_t sectors, const char *dev);
int dmsetup_remove(const char *mapper_name);

int dm_integrity_status_get(const char *dev_name, struct block_device *dev);

int dm_sb_zeroize(const char *device);
int dm_create(const char *mapper_name, uint64_t sectors, const char *dev, bool wait);
int dm_remove(const char *mapper_name);

#endif /* USERFS_DM_H */
