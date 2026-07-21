/*
 * Copyright (c) 2026 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dm.h"
#include "ext4.h"
#include "stack.h"
#include "userfs.h"

#include <errno.h>
#include <string.h>

int partition_stack_setup(struct partition_stack *stack, bool force)
{
    ASSERT(stack->part->fs_probed, "Partition must be probed before setting up a stack");

    const char *current_path = stack->part->path;

    for (int i = 0; i < stack->nlayers; i++) {
        struct layer *l = &stack->layers[i];
        int ret;

        switch (l->type) {

        case LAYER_DM_INTEGRITY: {
            bool already_mapped = (dm_integrity_status_get(l->mapper_name, &l->dev) == 0);

            if (already_mapped && force) {
                ret = dm_remove(l->mapper_name);
                if (ret != 0) {
                    ERR("Failed to remove existing dm-integrity mapper '%s'\n",
                        l->mapper_name);
                    return ret;
                }
                already_mapped = false;
            }

            if (!already_mapped) {
                bool reset_sb = (stack->part->fs_info.type != FS_TYPE_INTEGRITY) || force;
                ret = dm_integrity_map(l->mapper_name, current_path, &l->dev, reset_sb);
                if (ret != 0) {
                    ERR("Failed to map dm-integrity '%s'\n", l->mapper_name);
                    return ret;
                }
            }

            mapper_info_display(&l->dev);
            current_path = l->dev.path;
            break;
        }

        case LAYER_FS_EXT4:
            ret = format_ext4(current_path, force);
            if (ret != 0) {
                ERR("Failed to format ext4 on '%s': %s\n", current_path, strerror(errno));
                return ret;
            }
            break;

        case LAYER_FS_BTRFS:
            ERR("BTRFS filesystem layer not yet implemented in stack\n");
            return -ENOSYS;

        case LAYER_DM_CRYPT:
            ERR("dm-crypt layer not yet implemented in stack\n");
            return -ENOSYS;

        default:
            ERR("Unknown layer type %d in stack '%s'\n", l->type, stack->name);
            return -EINVAL;
        }
    }

    return 0;
}

int partition_stack_teardown(struct partition_stack *stack, bool wipe)
{
    int ret = 0;

    /* Wait for udev to finish processing any filesystem probe events
     * (e.g. by-uuid symlink updates after mkfs) before removing dm
     * targets, otherwise dm_remove() may fail with EBUSY. */
    udev_settle();

    /* Remove dm layers in reverse order; skip filesystem layers. */
    for (int i = stack->nlayers - 1; i >= 0; i--) {
        struct layer *l = &stack->layers[i];

        switch (l->type) {
        case LAYER_DM_INTEGRITY:
        case LAYER_DM_CRYPT: {
            int r = dm_remove(l->mapper_name);
            if (r != 0) {
                ERR("Failed to remove dm mapper '%s'\n", l->mapper_name);
                ret = r;
            }
            break;
        }

        case LAYER_FS_EXT4:
        case LAYER_FS_BTRFS:
            /* Nothing to undo for filesystem layers. */
            break;

        default:
            ERR("Unknown layer type %d during teardown of '%s'\n", l->type, stack->name);
            break;
        }
    }

    if (wipe) {
        int r = dm_sb_zeroize(stack->part->path);
        if (r != 0) {
            ERR("Failed to zeroize superblock on '%s'\n", stack->part->path);
            ret = r;
        }
    }

    return ret;
}
