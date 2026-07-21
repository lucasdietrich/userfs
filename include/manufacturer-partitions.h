/*
 * Copyright (c) 2026 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USERFS_MANUFACTURER_PARTITIONS_H
#define USERFS_MANUFACTURER_PARTITIONS_H

#include "disk.h"

int setup_manufacturer_data(struct part_info *part, bool force, struct block_device *dmintegrity);
int clear_manufacturer_data(struct part_info *part, bool clear_integrity_sb);

#endif /* USERFS_MANUFACTURER_PARTITIONS_H */
