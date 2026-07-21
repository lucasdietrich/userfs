/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
 
#ifndef USERFS_UTILS_H
#define USERFS_UTILS_H

#include <stddef.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SECTOR_SIZE 512
#define KB          (1024)
#define MB          (1024 * KB)
#define GB          (1024 * MB)

void hexdump(const void *data, size_t size);

int create_directory(const char *dir);

int command_run(char *buf, size_t *buflen, const char *program, const char *argv[]);

#endif /* USERFS_UTILS_H */