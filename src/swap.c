#include "disk.h"
#include "fs.h"
#include "userfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <blkid.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

int format_swap_partition(struct part_info *part)
{
    return 0;
    int ret = -1;

    ASSERT(part->fs_probed == true, "Partition must be probed before formatting\n");

    bool do_format_swap = false;
    switch (part->fs_info.type) {
    case FS_TYPE_SWAP:
        LOG("Swap partition already formatted, skipping\n");
        break;
    case FS_TYPE_UNKNOWN:
    default:
        do_format_swap = true;
        break;
    }

    if (do_format_swap) {
        const char *mkswap_args[] = {
            "/sbin/mkswap",
            part->path,
            NULL,
        };

        ret = command_run(NULL, NULL, mkswap_args[0], mkswap_args);
        LOG("mkswap returned: %d\n", ret);
        if (ret < 0) {
            ERR("Failed to create swap space: %s\n", strerror(errno));
            goto exit;
        }

        LOG("Swap space created successfully on %s\n", part->path);
    }

    ret = 0;
exit:
    return ret;
}
