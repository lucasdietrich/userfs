/*
 * Copyright (c) 2026 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USERFS_STACK_H
#define USERFS_STACK_H

#include <stdbool.h>

#include "disk.h"

/**
 * Maximum number of layers in a partition stack.
 */
#define PARTITION_STACK_MAX_LAYERS 4

/**
 * Types of layers that can be stacked on top of a raw partition.
 *
 * Block-device transformation layers (dm-*) each produce a new virtual block
 * device that the next layer consumes. Filesystem layers are terminal: they
 * format the topmost block device and produce no further device.
 */
enum layer_type {
    LAYER_DM_INTEGRITY, /* dm-integrity: data integrity verification   */
    LAYER_DM_CRYPT,     /* dm-crypt:     transparent encryption (TODO)  */
    LAYER_FS_EXT4,      /* ext4 filesystem format (terminal)           */
    LAYER_FS_BTRFS,     /* btrfs filesystem format (terminal, TODO)    */
};

/**
 * A single layer in a partition stack.
 *
 * @type         Layer type (see enum layer_type).
 * @mapper_name  Name of the device-mapper target (dm layers only).
 * @dev          Virtual block device produced by this layer after setup
 *               (dm layers only; filled by partition_stack_setup()).
 */
struct layer {
    enum layer_type    type;
    const char        *mapper_name;
    struct block_device dev; /* filled by partition_stack_setup() */
};

/**
 * A partition stack: an ordered sequence of layers applied to a raw partition.
 *
 * Layers are applied in index order during setup and reversed during teardown.
 * Filesystem layers (LAYER_FS_*) must be placed last and are no-ops during
 * teardown.
 *
 * @part    Raw underlying partition.
 * @name    Human-readable label used in log messages.
 * @nlayers Number of valid entries in @layers.
 * @layers  Layer configurations.
 */
struct partition_stack {
    struct part_info *part;
    const char       *name;
    int               nlayers;
    struct layer      layers[PARTITION_STACK_MAX_LAYERS];
};

/**
 * Setup a partition stack.
 *
 * Applies dm layers in order (creating device-mapper targets), then formats
 * the filesystem on the topmost device.  If @force is true, any existing
 * dm targets are removed and recreated, and the filesystem is reformatted.
 *
 * @stack  Stack configuration. dm layer @dev fields are filled on success.
 * @force  Recreate dm targets and reformat filesystem even if already present.
 * @return 0 on success, negative errno-like value on failure.
 */
int partition_stack_setup(struct partition_stack *stack, bool force);

/**
 * Teardown a partition stack.
 *
 * Removes dm layers in reverse order. Filesystem layers are silently skipped.
 * If @wipe is true, the raw partition's superblock is zeroed after the dm
 * layers are removed (destroying integrity / crypt metadata).
 *
 * @stack  Stack configuration. Layer @dev fields need not be populated.
 * @wipe   Zeroize the superblock of the underlying raw partition.
 * @return 0 on success, negative errno-like value on failure (all layers are
 *         still attempted even when an intermediate step fails).
 */
int partition_stack_teardown(struct partition_stack *stack, bool wipe);

#endif /* USERFS_STACK_H */
