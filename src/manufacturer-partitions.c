/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "manufacturer-partitions.h"
#include "stack.h"
#include "userfs.h"

int setup_manufacturer_data(struct part_info *part,
                            bool force,
                            struct block_device *dmintegrity)
{
    struct partition_stack stack = {
        .part    = part,
        .name    = "manufacturer",
        .nlayers = 1,
        .layers  = {
             {.type = LAYER_DM_INTEGRITY, .mapper_name = MANUFACTURER_MAPPER_NAME},
        }};

    int ret = partition_stack_setup(&stack, force);
    if (ret == 0 && dmintegrity)
        *dmintegrity = stack.layers[0].dev;
    return ret;
}

int clear_manufacturer_data(struct part_info *part, bool clear_integrity_sb)
{
    struct partition_stack stack = {
        .part    = part,
        .name    = "manufacturer",
        .nlayers = 2,
        .layers  = {
             {.type = LAYER_DM_INTEGRITY, .mapper_name = MANUFACTURER_MAPPER_NAME},
             {.type = LAYER_FS_EXT4},
        }};
    return partition_stack_teardown(&stack, clear_integrity_sb);
}