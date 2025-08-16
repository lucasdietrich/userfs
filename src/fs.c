#include <blkid.h>
#include <fcntl.h>
#include <unistd.h>

#include "userfs.h"

int fs_probe(const char *part_device, struct fs_info *info)
{
    int ret        = -1;
    int fd         = -1;
    blkid_probe pr = NULL;

    if (!part_device || !info) {
        fprintf(stderr, "Invalid arguments for fs_probe\n");
        goto exit;
    }

    // Clear the fs_info structure
    memset(info, 0, sizeof(struct fs_info));

    pr = blkid_new_probe();
    if (!pr) {
        fprintf(stderr, "Failed to create blkid probe\n");
        goto exit;
    }

    fd = open(part_device, 0); // Read-only mode
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "Failed to open partition device\n");
        goto exit;
    }

    // offset = 0, size = 0 (whole device)
    ret = blkid_probe_set_device(pr, fd, 0, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to set device for blkid probe: %s\n", strerror(errno));
        goto exit;
    }

    // All code before this point could be replace with
    // blkid_new_probe_from_filename(part_device);

    blkid_probe_enable_partitions(pr, true);
    blkid_probe_enable_superblocks(pr, true);
    blkid_probe_set_superblocks_flags(
        pr, BLKID_SUBLKS_UUID | BLKID_SUBLKS_LABEL | BLKID_SUBLKS_TYPE);

    ret = blkid_do_safeprobe(pr);
    if (ret < 0) {
        fprintf(stderr, "blkid_do_safeprobe failed: %s\n", strerror(errno));
        goto exit;
    }

    const char *fs_uuid = NULL;
    const char *type    = NULL;
    blkid_probe_lookup_value(pr, "UUID", &fs_uuid, NULL);
    blkid_probe_lookup_value(pr, "TYPE", &type, NULL);

    if (fs_uuid) {
        strncpy(info->uuid, fs_uuid, sizeof(info->uuid) - 1);
        info->uuid[sizeof(info->uuid) - 1] = '\0'; // Ensure null termination
    }

    if (type) {
        if (strcmp(type, "btrfs") == 0) {
            info->type = FS_TYPE_BTRFS;
        } else if (strcmp(type, "ext4") == 0) {
            info->type = FS_TYPE_EXT4;
        } else if (strcmp(type, "swap") == 0) {
            info->type = FS_TYPE_SWAP;
        } else {
            info->type = FS_TYPE_UNKNOWN;
        }
    }

    if (close(fd) < 0) {
        perror("close");
        fd = -1; // Prevent double close in exit block
        fprintf(stderr, "Failed to close partition device\n");
        goto exit;
    }
    fd = -1; // Mark as closed

    blkid_free_probe(pr);
    pr = NULL;

    return 0;

exit:
    if (pr) blkid_free_probe(pr);
    if (fd >= 0) close(fd);
    return ret;
}

static const char *fs_type_to_string(enum fs_type type)
{
    switch (type) {
    case FS_TYPE_BTRFS:
        return "btrfs";
    case FS_TYPE_EXT4:
        return "ext4";
    case FS_TYPE_SWAP:
        return "swap";
    case FS_TYPE_UNKNOWN:
    default:
        return "unknown";
    }
}

void fs_info_display(const struct fs_info *info)
{
    if (!info) return;

    printf("Filesystem Info:\n");
    printf("  Type: %s\n", fs_type_to_string(info->type));
    printf("  UUID: %s\n", info->uuid[0] ? info->uuid : "Not set");
}
