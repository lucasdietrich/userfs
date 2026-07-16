#include "userfs.h"

#include <blkid.h>
#include <fcntl.h>
#include <unistd.h>

static const char *fs_type_to_string(enum fs_type type)
{
    switch (type) {
    case FS_TYPE_BTRFS:
        return "btrfs";
    case FS_TYPE_EXT4:
        return "ext4";
    case FS_TYPE_SWAP:
        return "swap";
    case FS_TYPE_VFAT:
        return "vfat";
    case FS_TYPE_LVM:
        return "LVM2";
    case FS_TYPE_INTEGRITY:
        return "DM_integrity";
    case FS_TYPE_UNKNOWN:
    default:
        return "unknown";
    }
}

int fs_probe(const char *part_device, struct fs_info *info)
{
    int ret        = -1;
    int fd         = -1;
    blkid_probe pr = NULL;

    if (!part_device || !info) {
        ERR("Invalid arguments for fs_probe\n");
        goto exit;
    }

    // Clear the fs_info structure
    memset(info, 0, sizeof(struct fs_info));

    pr = blkid_new_probe();
    if (!pr) {
        ERR("Failed to create blkid probe\n");
        goto exit;
    }

    fd = open(part_device, 0); // Read-only mode
    if (fd < 0) {
        perror("open");
        ERR("Failed to open partition device\n");
        goto exit;
    }

    // offset = 0, size = 0 (whole device)
    ret = blkid_probe_set_device(pr, fd, 0, 0);
    if (ret < 0) {
        ERR("Failed to set device for blkid probe: %s\n", strerror(errno));
        goto exit;
    }

    ASSERT(blkid_probe_enable_partitions(pr, true) == 0,
           "Failed to enable partition probing");
    ASSERT(blkid_probe_enable_superblocks(pr, true) == 0,
           "Failed to enable superblock probing");

#if USERFS_PARTITION_TABLE_GPT
    ASSERT(blkid_probe_set_partitions_flags(pr, BLKID_PARTS_ENTRY_DETAILS) == 0,
           "Failed to set partition flags");
    ASSERT(blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_DEFAULT) == 0,
           "Failed to set superblock flags");
#elif USERFS_PARTITION_TABLE_DOS
    ASSERT(blkid_probe_set_superblocks_flags(
               pr, BLKID_SUBLKS_UUID | BLKID_SUBLKS_LABEL | BLKID_SUBLKS_TYPE) == 0,
           "Failed to set superblock flags");
#endif

    ret = blkid_do_safeprobe(pr);
    if (ret < 0) {
        ERR("blkid_do_safeprobe failed: %s\n", strerror(errno));
        goto exit;
    }

    const char *fs_uuid    = NULL;
    const char *type       = NULL;
    const char *part_label = NULL;
    size_t len             = 0;

    (void)blkid_probe_lookup_value(pr, "TYPE", &type, NULL);
    if (type) {
        if (strcmp(type, "btrfs") == 0) {
            info->type = FS_TYPE_BTRFS;
        } else if (strcmp(type, "ext4") == 0) {
            info->type = FS_TYPE_EXT4;
        } else if (strcmp(type, "swap") == 0) {
            info->type = FS_TYPE_SWAP;
        } else if (strcmp(type, "vfat") == 0) {
            info->type = FS_TYPE_VFAT;
        } else if (strcmp(type, "LVM2_member") == 0) {
            info->type = FS_TYPE_LVM;
        } else if (strcmp(type, "DM_integrity") == 0) {
            info->type = FS_TYPE_INTEGRITY;
        } else {
            info->type = FS_TYPE_UNKNOWN;
            LOG("\tUnknown filesystem type: %s\n", type);
        }
    }

    (void)blkid_probe_lookup_value(pr, "UUID", &fs_uuid, NULL);
    if (fs_uuid) {
        strncpy(info->uuid, fs_uuid, sizeof(info->uuid) - 1);
        info->uuid[sizeof(info->uuid) - 1] = '\0'; // Ensure null termination
    }

#if USERFS_PARTITION_TABLE_GPT
    (void)blkid_probe_lookup_value(pr, "PART_ENTRY_NAME", &part_label, &len);
    if (!part_label) {
        (void)blkid_probe_lookup_value(pr, "PARTLABEL", &part_label, &len);
    }

    if (part_label) {
        strncpy(info->part_label, part_label, sizeof(info->part_label) - 1);
        info->part_label[sizeof(info->part_label) - 1] = '\0'; // Ensure null termination
    }
#endif

    LOG("[ part %-18s ] PARTLABEL: %-16s",
        part_device,
        info->part_label[0] ? info->part_label : "-");
    if (info->uuid[0]) LOG(" UUID: %-36s", info->uuid);
    if (info->type != FS_TYPE_UNKNOWN) LOG(" type: %s", fs_type_to_string(info->type));
    LOG("\n");

    ret = 0;

exit:
    if (pr) blkid_free_probe(pr);
    if (fd >= 0) close(fd);
    return ret;
}

void fs_info_display(const struct fs_info *info)
{
    if (!info) return;

    LOG("[ fs ] type: %-8s PARTLABEL: %-16s UUID: %-36s\n",
        fs_type_to_string(info->type),
        info->part_label[0] ? info->part_label : "-",
        info->uuid[0] ? info->uuid : "-");
}

void fs_info_display_inline(const struct fs_info *info)
{
    if (!info) return;

    LOG("type: %-8s PARTLABEL: %-16s UUID: %-36s\n",
        fs_type_to_string(info->type),
        info->part_label[0] ? info->part_label : "-",
        info->uuid[0] ? info->uuid : "-");
}
