/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "manufacturer-partitions.h"
#include "stack.h"
#include "userfs.h"

int setup_manufacturer_data(struct part_info *part, struct block_device *dmintegrity)
{
    struct partition_stack stack = {
        .part    = part,
        .name    = "manufacturer",
        .nlayers = 1,
        .layers  = {
             {.type = LAYER_DM_INTEGRITY, .mapper_name = MANUFACTURER_MAPPER_NAME},
        }};

    int ret = partition_stack_setup(&stack, false);
    if (ret == 0 && dmintegrity)
        *dmintegrity = stack.layers[0].dev;
    return ret;
}
