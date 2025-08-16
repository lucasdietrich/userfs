#ifndef USERFS_OVERLAYS_H
#define USERFS_OVERLAYS_H

#include <stddef.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "userfs.h"

int step3_create_overlayfs(struct args *args);

#endif /* USERFS_OVERLAYS_H */