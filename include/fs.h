/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
 
#ifndef USERFS_FS_H
#define USERFS_FS_H

#include <stddef.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "disk.h"

int fs_probe(const char *part_device, struct fs_info *info);

void fs_info_display(const struct fs_info *info);

#endif /* USERFS_FS_H */