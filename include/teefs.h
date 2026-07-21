/*
 * Copyright (c) 2026 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USERFS_TEEFS_H
#define USERFS_TEEFS_H

#include "disk.h"
#include "userfs.h"

int setup_teefs(struct part_info *part, bool force, struct block_device *dmintegrity);

int clear_teefs(struct part_info *part, bool clear_integrity_sb);

#endif /* USERFS_TEEFS_H */
