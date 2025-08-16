#include "disk.h"
#include "fs.h"
#include "userfs.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <blkid.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

int step4_format_swap_partition(struct args *args, struct disk_info *disk, size_t swap_partno)
{
    (void)args;
    int ret = -1;

    if (swap_partno >= disk->partition_count) {
        fprintf(stderr, "Invalid swap partition number: %d\n", swap_partno);
        goto exit;
    }

    char swap_part_device[PATH_MAX];
    ret = disk_part_build_path(swap_part_device, sizeof(swap_part_device), swap_partno);
    if (ret < 0) {
        fprintf(stderr, "Failed to build swap partition path: %s\n", strerror(errno));
        goto exit;
    }

    printf("Formatting swap partition %zu (%s)\n", swap_partno, swap_part_device);

    struct part_info *swap_part = &disk->partitions[swap_partno];
    ret                         = fs_probe(swap_part_device, &swap_part->fs_info);
    if (ret < 0) {
        fprintf(stderr, "Failed to probe swap partition: %s\n", strerror(errno));
        goto exit;
    }

    fs_info_display(&swap_part->fs_info);

    bool do_format_swap = false;
    switch (swap_part->fs_info.type) {
    case FS_TYPE_SWAP:
        printf("Swap partition %zu already formatted, skipping\n", swap_partno);
        break;
    case FS_TYPE_UNKNOWN:
    default:
        do_format_swap = true;
        break;
    }

    if (do_format_swap) {
        const char *const mkswap_args[] = {
            "mkswap",
            swap_part_device,
            NULL,
        };

        command_display(mkswap_args[0], (char *const *)mkswap_args);
        ret = command_run(NULL, NULL, mkswap_args[0], (char *const *)mkswap_args);
        LOG("mkswap returned: %d\n", ret);
        if (ret < 0) {
            fprintf(stderr, "Failed to create swap space: %s\n", strerror(errno));
            goto exit;
        }

        LOG("Swap space created successfully on %s\n", swap_part_device);
    }

    ret = 0;
exit:
    return ret;
}
