#ifndef USERFS_BTRFS_H
#define USERFS_BTRFS_H

#include <stddef.h>

#include "userfs.h"

#define BTRFS_SV_DATA_INDEX   0
#define BTRFS_SV_CONFIG_INDEX 1

const char *btrfs_get_volume(size_t sv_index);

int step2_create_btrfs_filesystem(struct args *args, struct part_info *userfs_part);

#endif /* USERFS_BTRFS_H */