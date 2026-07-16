/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "disk.h"
#include "fs.h"
#include "userfs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <blkid.h>
#include <fcntl.h>
#include <libfdisk.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DO_BLKID_PROBE 1

#if defined(USERFS_PART_NO) && (USERFS_PART_NO >= MAX_SUPPORTED_PARTITIONS)
#error "USERFS_PART_NO exceeds maximum supported partitions"
#endif

#define PARTTYPE_CODE_FAT32_LBA 0x0C
#define PARTTYPE_CODE_LINUX     0x83
#define PARTTYPE_CODE_SWAP      0x82
#define PARTTYPE_CODE_EXTENDED  0x05

#define DOS_LOGICAL_VOLUME_HEADER_SIZE 2048

#define RO_ENABLED 0

#define USERFS_PART_CODE PARTTYPE_CODE_LINUX

#define USERFS_MIN_SIZE_B (1llu * GB)
#define USERFS_MIN_SIZE_S (USERFS_MIN_SIZE_B / SECTOR_SIZE)

static int disk_get_size(const char *device, uint64_t *size)
{
    int ret = -1;
    int fd  = -1;

    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("open");
        ERR("Failed to open device\n");
        goto exit;
    }

    ret = ioctl(fd, BLKGETSIZE64, size);
    if (ret < 0) {
        perror("ioctl BLKGETSIZE64");
        ERR("Failed to get device size\n");
        goto exit;
    }

    ret = close(fd);
    if (ret < 0) {
        perror("close");
        ERR("Failed to close device\n");
        goto exit;
    }

    return 0;
exit:
    if (fd >= 0) close(fd);
    return ret;
}

static int disk_read_partitions(struct fdisk_context *ctx,
                                struct fdisk_label *label,
                                struct disk_info *disk,
                                const char *device,
                                bool do_blkid_probe)
{
    disk->type          = fdisk_label_get_type(label);
    disk->total_sectors = fdisk_get_nsectors(ctx);
    disk->total_size    = (uint64_t)disk->total_sectors * SECTOR_SIZE;

    size_t actual_partition_count = fdisk_get_npartitions(ctx);
    disk->partition_count         = (actual_partition_count > MAX_SUPPORTED_PARTITIONS)
                                        ? MAX_SUPPORTED_PARTITIONS
                                        : actual_partition_count;

    struct fdisk_partition *part = NULL;
    for (size_t indox = 0; indox < MAX_SUPPORTED_PARTITIONS; indox++) {
        struct part_info *pinfo = &disk->partitions[indox];

        pinfo->partno = indox;
        pinfo->start  = 0;
        pinfo->end    = 0;
        pinfo->size   = 0;
        pinfo->used   = fdisk_is_partition_used(ctx, indox);

        if (!pinfo->used) continue;

        if (fdisk_get_partition(ctx, indox, &part) < 0) continue;

        struct fdisk_parttype *pt = fdisk_partition_get_type(part);
        if (!pt) continue;

        pinfo->start     = fdisk_partition_get_start(part);
        pinfo->end       = fdisk_partition_get_end(part);
        pinfo->size      = fdisk_partition_get_size(part);
        pinfo->partno    = fdisk_partition_get_partno(part);
        pinfo->type      = fdisk_parttype_get_code(pt);
        pinfo->type_name = fdisk_parttype_get_name(pt);

        ASSERT(indox == pinfo->partno, "Partition index must match partition number");

        if (do_blkid_probe) {
            // inspect the partition info after changes
            char dev[PATH_MAX];
            int ret = disk_part_build_path(device, dev, sizeof(dev), pinfo->partno);
            if (ret < 0) {
                fprintf(stderr,
                        "Failed to build userfs partition path: %s\n",
                        strerror(errno));
                return ret;
            }

            ret = fs_probe(dev, &pinfo->fs_info);
            if (ret != 0) {
                fprintf(stderr,
                        "Failed to probe filesystem on %s: %s\n",
                        dev,
                        strerror(errno));
                return ret;
            }
            pinfo->fs_probed = true;
        } else {
            pinfo->fs_probed = false;
        }
    }

    disk->last_used_partno = 0;
    for (size_t partno = 0; partno < MAX_SUPPORTED_PARTITIONS; partno++) {
        if (disk->partitions[partno].used) disk->last_used_partno = partno;
    }

    disk->partition_count  = disk->last_used_partno + 1;
    disk->next_free_sector = disk->partitions[disk->last_used_partno].end + 1;
    disk->free_sectors     = disk->total_sectors - disk->next_free_sector;
    disk->free_size        = (uint64_t)disk->free_sectors * SECTOR_SIZE;

    return 0;
}

static const char *format_size(uint64_t bytes, char *buf, size_t len)
{
    if (bytes >= MB)
        snprintf(buf, len, "%lluMB", (unsigned long long)(bytes / MB));
    else if (bytes >= KB)
        snprintf(buf, len, "%lluKB", (unsigned long long)(bytes / KB));
    else
        snprintf(buf, len, "%lluB", (unsigned long long)bytes);
    return buf;
}

static const char *disk_label_type_to_string(enum fdisk_labeltype type)
{
    switch (type) {
    case FDISK_DISKLABEL_DOS:
        return "DOS";
    case FDISK_DISKLABEL_GPT:
        return "GPT";
    case FDISK_DISKLABEL_SUN:
        return "SUN";
    case FDISK_DISKLABEL_SGI:
        return "SGI";
    default:
        return "unknown";
    }
}

static void disk_display_info(const struct disk_info *disk)
{
    LOG("[ disk ] type: %s sectors: %llu sector_size: %uB total: %lluMB free: %lluMB parts: %zu\n",
        disk_label_type_to_string(disk->type),
        (unsigned long long)disk->total_sectors,
        SECTOR_SIZE,
        (unsigned long long)(disk->total_size / MB),
        (unsigned long long)(disk->free_size / MB),
        disk->partition_count);

    for (size_t n = 0; n < disk->partition_count; n++) {
        const struct part_info *pinfo = &disk->partitions[n];

        if (!pinfo->used) continue;

        char size_str[32];
        format_size(pinfo->size * SECTOR_SIZE, size_str, sizeof(size_str));

        LOG("[ part %2zu ] %-20s (0x%02x) start: %-12llu end: %-12llu size: %s\n",
            pinfo->partno,
            pinfo->type_name ? pinfo->type_name : "?",
            pinfo->type,
            (unsigned long long)pinfo->start,
            (unsigned long long)pinfo->end,
            size_str);
    }
}

static int
disk_add_part(struct fdisk_context *ctx, struct fdisk_label *label, struct part_info *new)
{
    printf("Adding partition: %zu start: %llu end: %llu size: %llu\n",
           new->partno,
           (unsigned long long)new->start,
           (unsigned long long)new->end,
           (unsigned long long)new->size);

    int ret                   = -1;
    struct fdisk_parttype *pt = NULL;

    struct fdisk_partition *part = fdisk_new_partition();
    if (!part) {
        ERR("Failed to create new partition\n");
        goto exit;
    }

    fdisk_partition_set_partno(part, new->partno);
    fdisk_partition_set_start(part, new->start);
    fdisk_partition_set_size(part, new->size);

    if (new->part_label) fdisk_partition_set_name(part, new->part_label);

    pt = fdisk_label_get_parttype_from_code(label, new->type);
    if (!pt) {
        ERR("Failed to get partition type\n");
        goto exit;
    }

    fdisk_partition_set_type(part, pt);

    size_t cur_partno = (size_t)-1;
    ret               = fdisk_add_partition(ctx, part, &cur_partno);
    if (ret != 0) {
        ERR("Failed to add partition\n");
        goto exit;
    }

    ASSERT(cur_partno == new->partno, "Partition number mismatch after adding partition");

exit:
    if (part) fdisk_unref_partition(part);
    if (pt) fdisk_unref_parttype(pt);
    return ret;
}

#if USERFS_PARTITION_TABLE_DOS
/* Create a new primary partition on the disk, using the rest of the free space */
static int disk_dos_add_userfs_as_new_primary_partition(struct fdisk_context *ctx,
                                                        struct fdisk_label *label,
                                                        struct disk_info *disk)
{
    ASSERT(disk->type == FDISK_DISKLABEL_DOS, "Only DOS partition tables are supported");
    ASSERT(disk->last_used_partno < 3, "We expect 3 or less primary partitions");

    int ret;
    struct part_info *prev = &disk->partitions[disk->last_used_partno];
    struct part_info *new  = &disk->partitions[disk->last_used_partno + 1u];

    new->start = disk->next_free_sector;
    new->end   = disk->total_sectors - 1;
    new->size  = disk->free_sectors;
    new->used  = 1;
    new->type  = USERFS_PART_CODE;

    LOG("Creating userfs partition: start=%llu, end=%llu, size=%llu\n",
        (unsigned long long)new->start,
        (unsigned long long)new->end,
        (unsigned long long)new->size);

    ASSERT(prev->end + 1 == new->start,
           "Previous partition end does not match current partition start");

    ASSERT(new->end - new->start + 1 == new->size,
           "Partition size does not match start and end");

    ret = disk_add_part(ctx, label, new);
    if (ret != 0) {
        ERR("Failed to add userfs partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk, NULL, false);
    ASSERT(ret == 0, "Failed to read partitions after deletion");
    disk_display_info(disk);

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        ERR("Failed to write disk label\n");
        goto exit;
    }

exit:
    return ret;
}

/* Create a new extended partition on the disk, using the rest of the free space */
static int disk_dos_extend_partition_add_userfs(struct fdisk_context *ctx,
                                                struct fdisk_label *label,
                                                struct disk_info *disk)
{
    ASSERT(disk->type == FDISK_DISKLABEL_DOS, "Only DOS partition tables are supported");
    ASSERT(disk->partition_count == MAX_DOS_PARTITIONS, "we expect 4 primary partitions");
    ASSERT(disk->last_used_partno == 3, "we expect all partitions to be used");
    ASSERT(disk->partitions[3].type != PARTTYPE_CODE_EXTENDED,
           "we expect the last partition to be a primary one");

    int ret;

    /* Save previous last partition information */
    struct part_info *old = &disk->partitions[3];
    size_t old_size       = old->size;
    int old_type          = old->type;

    // Delete the old 4th partition
    printf("Deleting partition %d\n", partno);
    ret = fdisk_delete_partition(ctx, old->partno);
    if (ret != 0) {
        ERR("Failed to delete old 4th partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk, NULL, false);
    ASSERT(ret == 0, "Failed to read partitions after deletion");
    disk_display_info(disk);

    struct part_info *ext   = &disk->partitions[3];
    struct part_info *moved = &disk->partitions[4];
    struct part_info *new   = &disk->partitions[5];

    ext->partno = 3u;
    ext->used   = 1;
    ext->start  = disk->next_free_sector;
    ext->end    = disk->total_sectors - 1;
    ext->size   = disk->free_sectors;
    ext->type   = PARTTYPE_CODE_EXTENDED;

    ret = disk_add_part(ctx, label, ext);
    if (ret != 0) {
        ERR("Failed to add extended partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk, NULL, false);
    ASSERT(ret == 0, "Failed to read partitions after deletion");
    disk_display_info(disk);

    moved->partno = 4u;
    moved->used   = 1;
    moved->start  = ext->start + DOS_LOGICAL_VOLUME_HEADER_SIZE;
    moved->end    = moved->start + old_size - 1;
    moved->size   = old_size;
    moved->type   = old_type;

    ret = disk_add_part(ctx, label, moved);
    if (ret != 0) {
        ERR("Failed to re-add moved partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk, NULL, false);
    ASSERT(ret == 0, "Failed to read partitions after moved add");
    disk_display_info(disk);

    new->partno = 5u;
    new->used   = 1;
    new->start  = moved->end + DOS_LOGICAL_VOLUME_HEADER_SIZE + 1u;
    new->end    = ext->end;
    new->size   = new->end - new->start + 1;
    new->type   = USERFS_PART_CODE;

    ret = disk_add_part(ctx, label, new);
    if (ret != 0) {
        ERR("Failed to add userfs partition\n");
        goto exit;
    }   

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk, NULL, false);
    ASSERT(ret == 0, "Failed to read partitions after userfs add");
    disk_display_info(disk);

    /* Tell there is no space left */
    disk->next_free_sector = disk->total_size;
    disk->free_sectors     = 0u;
    disk->free_size        = 0u;

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        ERR("Failed to write disk label\n");
        goto exit;
    }

exit:
    return ret;
}

/**
 * Create a userfs partition on the disk.
 *
 * This function creates a userfs partition using the remaining free space
 * on the disk. It assumes that the disk partition has been initialized and has enough
 * free space for the userfs partition.
 *
 * @param ctx The fdisk context.
 * @param label The fdisk label.
 * @param disk The disk information structure.
 * @param p The partition to create.
 * @return 0 on success, -1 on failure, 0 if partition was created, 1 if partition already
 * exists.
 */
static int disk_dos_create_userfs_partition(struct fdisk_context *ctx,
                                            struct fdisk_label *label,
                                            struct disk_info *disk,
                                            size_t desired_partno)
{
    int ret = -1;

    ASSERT(disk->type == FDISK_DISKLABEL_DOS, "Only DOS partition tables are supported");
    ASSERT(desired_partno >= 1, "Partition index must be >= 1");

    struct part_info *userfs = &disk->partitions[desired_partno];

    if (userfs->used) {
        ERR("Partition %zu is already defined\n", userfs->partno);
        return 1;
    }

    if (disk->free_sectors < USERFS_MIN_SIZE_S) {
        ERR("Not enough free space for userfs partition\n");
        goto exit;
    }

    if (desired_partno <= 3u) {
        /* Primary partitions */
        ret = disk_dos_add_userfs_as_new_primary_partition(ctx, label, disk);
        if (ret != 0) {
            ERR("Failed to create primary partition\n");
            goto exit;
        }

    } else if (desired_partno == 5u) {
        /* Need extended + logical partitions */
        ret = disk_dos_extend_partition_add_userfs(ctx, label, disk);
        if (ret != 0) {
            ERR("Failed to extend partition\n");
            goto exit;
        }
    } else {
        ERR("Unsupported partition number %zu\n", desired_partno);
        ret = -1;
        goto exit;
    }

exit:
    return ret;
}
#endif

#if USERFS_PARTITION_TABLE_GPT
/* Create a new primary partition on the disk, using the rest of the free space */
static int disk_gpt_add_userfs_partition(struct fdisk_context *ctx,
                                         struct fdisk_label *label,
                                         struct disk_info *disk)
{
    ASSERT(disk->type == FDISK_DISKLABEL_DOS, "Only DOS partition tables are supported");

    int ret;
    struct part_info *prev = &disk->partitions[disk->last_used_partno];
    struct part_info *new  = &disk->partitions[disk->last_used_partno + 1u];

    new->start      = disk->next_free_sector;
    new->end        = disk->total_sectors - 1;
    new->size       = disk->free_sectors;
    new->used       = 1;
    new->type       = USERFS_PART_CODE;
    new->part_label = USERFS_PART_LABEL;

    LOG("Creating userfs partition: start=%llu, end=%llu, size=%llu\n",
        (unsigned long long)new->start,
        (unsigned long long)new->end,
        (unsigned long long)new->size);

    ASSERT(prev->end + 1 == new->start,
           "Previous partition end does not match current partition start");

    ASSERT(new->end - new->start + 1 == new->size,
           "Partition size does not match start and end");

    ret = disk_add_part(ctx, label, new);
    if (ret != 0) {
        ERR("Failed to add userfs partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk, NULL, false);
    ASSERT(ret == 0, "Failed to read partitions after deletion");
    disk_display_info(disk);

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        ERR("Failed to write disk label\n");
        goto exit;
    }

exit:
    return ret;
}

struct part_info *disk_find_partition_by_label(struct disk_info *disk,
                                               const char *partlabel)
{
    for (size_t i = 0; i < disk->partition_count; i++) {
        struct part_info *pinfo = &disk->partitions[i];
        if (pinfo->used && strcmp(pinfo->fs_info.part_label, partlabel) == 0) {
            return pinfo;
        }
    }
    return NULL;
}

/**
 * Create a partition on the disk if it does not already exist.
 *
 * This function creates a userfs partition using the remaining free space
 * on the disk. It assumes that the disk partition has been initialized and has enough
 * free space for the userfs partition.
 *
 * @param ctx The fdisk context.
 * @param label The fdisk label.
 * @param disk The disk information structure.
 * @param partlabel The partition label to assign.
 * @return 0 on success, -1 on failure, 0 if partition was created, 1 if partition already
 * exists.
 */
static int disk_gpt_create_partition_if_not_exist(struct fdisk_context *ctx,
                                                  struct fdisk_label *label,
                                                  struct disk_info *disk,
                                                  const char *partlabel)
{
    int ret = -1;

    struct part_info *existing = disk_find_partition_by_label(disk, partlabel);
    if (existing) {
        LOG("Partition with label '%s' already exists as partition number %zu\n",
            partlabel,
            existing->partno);
        return 0;
    }

    size_t desired_partno = disk->last_used_partno + 1u;
    LOG("Creating partition with label '%s' as partition number %zu\n",
        partlabel,
        desired_partno);

    ASSERT(desired_partno < MAX_SUPPORTED_PARTITIONS, "No more partition slots available");

    struct part_info *userfs_part = &disk->partitions[desired_partno];

    if (userfs_part->used) {
        ERR("Partition %zu is already defined\n", userfs_part->partno);
        return 1;
    }

    if (disk->free_sectors < USERFS_MIN_SIZE_S) {
        ERR("Not enough free space for userfs partition\n");
        goto exit;
    }

    /* Add GPT partition */
    ret = disk_gpt_add_userfs_partition(ctx, label, disk);
    if (ret != 0) {
        ERR("Failed to create primary partition\n");
        goto exit;
    }

exit:
    return ret;
}
#endif

void disk_clear_info(struct disk_info *disk)
{
    memset(disk, 0, sizeof(*disk));
}

static int disk_delete_userfs_partition(struct fdisk_context *ctx,
                                        struct part_info *pinfo)
{
    int ret = -1;

    if (!pinfo || !pinfo->used) {
        LOG("userfs partition is not in use or does not exist, nothing to delete\n");
        return 0;
    }

    LOG("Deleting userfs partition %zu\n", pinfo->partno);

    ret = fdisk_delete_partition(ctx, pinfo->partno);
    if (ret != 0) {
        ERR("Failed to delete partition %zu\n", pinfo->partno);
        return -1;
    }

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        ERR("Failed to write disk label after deletion\n");
        return -1;
    }

    // Update partition info
    memset(pinfo, 0, sizeof(*pinfo));

    LOG("Partition %zu deleted successfully\n", pinfo->partno);
    return 0;
}

/**
 * Notify the kernel of partition table changes, then re-read and display the updated
 * partition layout. Call this after any partition create/delete operation.
 */
static int disk_reload(struct fdisk_context *ctx,
                       struct fdisk_label *label,
                       struct disk_info *disk,
                       const char *device)
{
    int ret = disk_partprobe(device);
    if (ret < 0) {
        ERR("Failed to partprobe: %s\n", strerror(errno));
        return ret;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk, device, true);
    if (ret != 0) {
        ERR("Failed to read disk info after reload\n");
        return ret;
    }

    disk_display_info(disk);
    return 0;
}

int step1_create_userfs_partition(struct args *args, struct disk_info *disk)
{
    int ret                   = -1;
    uint64_t device_size      = 0;
    struct fdisk_context *ctx = NULL;
    struct fdisk_label *label = NULL;
    const char *device        = args->dev;

    fdisk_init_debug(0x0);
    blkid_init_debug(0x0);

    ctx = fdisk_new_context();
    if (!ctx) {
        ERR("Failed to create fdisk context\n");
        goto exit;
    }

    if (fdisk_assign_device(ctx, device, RO_ENABLED) < 0) {
        ERR("Failed to assign device\n");
        goto exit;
    }

    label = fdisk_get_label(ctx, "dos");
    if (!label) {
        ERR("Failed to get label\n");
        goto exit;
    }

    disk->type = fdisk_label_get_type(label);
    if (disk->type != FDISK_DISKLABEL_DOS) {
        ERR("Unsupported partition table type\n");
        goto exit;
    }

    /* Initial read + integrity check + display */
    ret = disk_read_partitions(ctx, label, disk, device, true);
    if (ret != 0) {
        ERR("Failed to read disk info\n");
        goto exit;
    }

    if (disk_get_size(device, &device_size) != 0) {
        ERR("Failed to get device size\n");
        goto exit;
    }
    ASSERT(device_size == disk->total_size,
           "Device size does not match total sectors * SECTOR_SIZE");

    disk_display_info(disk);

    /* Locate the userfs partition */
#if USERFS_PARTITION_TABLE_DOS
    struct part_info *userfs_part = &disk->partitions[USERFS_PART_NO];
    const char *userfs_label      = "-";
#elif USERFS_PARTITION_TABLE_GPT
    struct part_info *userfs_part = disk_find_partition_by_label(disk, USERFS_PART_LABEL);
    const char *userfs_label      = USERFS_PART_LABEL;
#endif
    bool partition_exists = (userfs_part && userfs_part->used);
    char partno_str[8];
    if (partition_exists)
        snprintf(partno_str, sizeof(partno_str), "%zu", userfs_part->partno);
    else
        snprintf(partno_str, sizeof(partno_str), "-");
    LOG("[ userfs ] label: %-16s partno: %-4s status: %s\n",
        userfs_label, partno_str, partition_exists ? "exists" : "not found");

    /* Handle delete request */
    if (args->flags & FLAG_USERFS_DELETE) {
        ret = disk_delete_userfs_partition(ctx, userfs_part);
        if (ret != 0) {
            ERR("Failed to delete userfs partition\n");
            goto exit;
        }

        ret = fdisk_deassign_device(ctx, 0);
        if (ret != 0) {
            ERR("Failed to deassign device\n");
            goto exit;
        }
        fdisk_unref_context(ctx);
        exit(EXIT_SUCCESS);
    }

    /* Create partition if missing (first boot) */
    if (!partition_exists) {
        if (args->flags & FLAG_USERFS_TRUST_RESIDENT) {
            printf("First boot: Trusting existing userfs partition without formatting\n");
        } else {
            printf("First boot: Userfs partition will be formatted to BTRFS\n");
            args->flags |= FLAG_USERFS_FORCE_FORMAT;
        }

#if USERFS_PARTITION_TABLE_DOS
        ret = disk_dos_create_userfs_partition(ctx, label, disk, USERFS_PART_NO);
#elif USERFS_PARTITION_TABLE_GPT
        ret = disk_gpt_create_partition_if_not_exist(ctx, label, disk, USERFS_PART_LABEL);
#endif
        if (ret != 0) {
            ERR("Failed to create userfs partition\n");
            goto exit;
        }

        /* Partition table changed — notify kernel and refresh */
        ret = disk_reload(ctx, label, disk, device);
        if (ret != 0) {
            ERR("Failed to reload disk after partition creation\n");
            goto exit;
        }
    }

    return 0;

exit:
    disk_clear_info(disk);
    fdisk_deassign_device(ctx, 0);
    fdisk_unref_context(ctx);
    return ret;
}

int disk_partprobe(const char *device)
{
    int ret;

    // FIXME: try another method to partprobe, the commented code below exit with error:
    // BLKRRPART: Device or resource busy

    // int fd = open(DISK, O_RDONLY);
    // if (fd >= 0) {
    //     ret = ioctl(fd, BLKRRPART); // Re-read partition table
    //     printf("BLKRRPART returned: %d %s\n", ret, strerror(errno));
    //     close(fd);
    //     sleep(1); // Wait for /dev/mmcblk0pX to appear
    // }

    char *const partprobe_args[] = {
        "partprobe",
        (char *)device,
        NULL,
    };
    ret = command_run(NULL, NULL, "partprobe", partprobe_args);

    return ret;
}

ssize_t disk_part_build_path(const char *device, char *buf, size_t buf_len, size_t partno)
{
    return snprintf(buf, buf_len, DISK_PART_FMT, device, partno + 1u);
}
