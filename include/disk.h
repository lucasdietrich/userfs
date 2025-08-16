#ifndef USERFS_DISK_H
#define USERFS_DISK_H

#include <stddef.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "userfs.h"

int disk_partprobe(const char *device);

void disk_clear_info(struct disk_info *disk);

int step1_create_userfs_partition(struct args *args, struct disk_info *disk);

#endif /* USERFS_DISK_H */