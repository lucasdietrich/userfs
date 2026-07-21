/*
 * Copyright (c) 2026 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USERFS_EXT4_H
#define USERFS_EXT4_H

#include <stdbool.h>
#include "disk.h"

int format_ext4(const char *device, bool force);

#endif /* USERFS_EXT4_H */