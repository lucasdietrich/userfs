/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "userfs.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <blkid.h>
#include <fcntl.h>
#include <libfdisk.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if USERFS_PART_NO >= MAX_SUPPORTED_PARTITIONS
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
        fprintf(stderr, "Failed to open device\n");
        goto exit;
    }

    ret = ioctl(fd, BLKGETSIZE64, size);
    if (ret < 0) {
        perror("ioctl BLKGETSIZE64");
        fprintf(stderr, "Failed to get device size\n");
        goto exit;
    }

    ret = close(fd);
    if (ret < 0) {
        perror("close");
        fprintf(stderr, "Failed to close device\n");
        goto exit;
    }

    return 0;
exit:
    if (fd >= 0) close(fd);
    return ret;
}

static int disk_read_partitions(struct fdisk_context *ctx,
                                struct fdisk_label *label,
                                struct disk_info *disk)
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
    }

    disk->last_used_partno = 0;
    for (size_t partno = 0; partno < MAX_SUPPORTED_PARTITIONS; partno++) {
        if (disk->partitions[partno].used) disk->last_used_partno = partno;
    }

    disk->next_free_sector = disk->partitions[disk->last_used_partno].end + 1;
    disk->free_sectors     = disk->total_sectors - disk->next_free_sector;
    disk->free_size        = (uint64_t)disk->free_sectors * SECTOR_SIZE;

    return 0;
}

static void disk_display_info(const struct disk_info *disk)
{
    LOG("Disk Information (type: %d, parts: %u)\n", disk->type, disk->partition_count);
    LOG("\tTotal: %llu sectors (%llu MB)\n", disk->total_sectors, disk->total_size / MB);
    LOG("\tFree: %u sectors (%llu MB)\n", disk->free_sectors, disk->free_size / MB);

    for (size_t n = 0; n < disk->partition_count; n++) {
        const struct part_info *pinfo = &disk->partitions[n];

        if (!pinfo->used) {
            continue;
        }

        uint64_t approx_size_mb = pinfo->size * SECTOR_SIZE / MB;

        LOG("[%zu] %s (%02zx) start: %llu end: %llu size: %llu (%llu MB)\n",
            pinfo->partno,
            pinfo->type_name,
            pinfo->type,
            (unsigned long long)pinfo->start,
            (unsigned long long)pinfo->end,
            (unsigned long long)pinfo->size,
            (unsigned long long)approx_size_mb);
    }
}

static int disk_delete_part(struct fdisk_context *ctx, int partno)
{
    printf("Deleting partition %d\n", partno);
    int ret;
    // Delete the old 4th partition
    ret = fdisk_delete_partition(ctx, partno);
    if (ret != 0) {
        fprintf(stderr, "Failed to delete old 4th partition\n");
    }

    return ret;
}

static int
disk_add_part(struct fdisk_context *ctx, struct fdisk_label *label, struct part_info *new)
{
    printf("Adding partition: %d start: %llu end: %llu size: %llu\n",
           new->partno,
           (unsigned long long)new->start,
           (unsigned long long)new->end,
           (unsigned long long)new->size);

    int ret                   = -1;
    struct fdisk_parttype *pt = NULL;

    struct fdisk_partition *part = fdisk_new_partition();
    if (!part) {
        fprintf(stderr, "Failed to create new partition\n");
        goto exit;
    }

    fdisk_partition_set_partno(part, new->partno);
    fdisk_partition_set_start(part, new->start);
    fdisk_partition_set_size(part, new->size);

    pt = fdisk_label_get_parttype_from_code(label, new->type);
    if (!pt) {
        fprintf(stderr, "Failed to get partition type\n");
        goto exit;
    }

    fdisk_partition_set_type(part, pt);

    size_t cur_partno = (size_t)-1;
    ret               = fdisk_add_partition(ctx, part, &cur_partno);
    if (ret != 0) {
        fprintf(stderr, "Failed to add partition\n");
        goto exit;
    }

    ASSERT(cur_partno == new->partno, "Partition number mismatch after adding partition");

exit:
    if (part) fdisk_unref_partition(part);
    if (pt) fdisk_unref_parttype(pt);
    return ret;
}

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
        fprintf(stderr, "Failed to add userfs partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk);
    ASSERT(ret == 0, "Failed to read partitions after deletion");
    disk_display_info(disk);

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to write disk label\n");
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
    ret                   = disk_delete_part(ctx, old->partno);
    if (ret != 0) {
        fprintf(stderr, "Failed to delete old partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk);
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
        fprintf(stderr, "Failed to add extended partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk);
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
        fprintf(stderr, "Failed to re-add moved partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk);
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
        fprintf(stderr, "Failed to add userfs partition\n");
        goto exit;
    }

    disk_clear_info(disk);
    ret = disk_read_partitions(ctx, label, disk);
    ASSERT(ret == 0, "Failed to read partitions after userfs add");
    disk_display_info(disk);

    /* Tell there is no space left */
    disk->next_free_sector = disk->total_size;
    disk->free_sectors     = 0u;
    disk->free_size        = 0u;

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
        fprintf(stderr, "Partition %zu is already defined\n", userfs->partno);
        return 1;
    }

    if (disk->free_sectors < USERFS_MIN_SIZE_S) {
        fprintf(stderr, "Not enough free space for userfs partition\n");
        goto exit;
    }

    if (desired_partno <= 3u) {
        /* Primary partitions */
        ret = disk_dos_add_userfs_as_new_primary_partition(ctx, label, disk);
        if (ret != 0) {
            fprintf(stderr, "Failed to create primary partition\n");
            goto exit;
        }

    } else if (desired_partno == 5u) {
        /* Need extended + logical partitions */
        ret = disk_dos_extend_partition_add_userfs(ctx, label, disk);
        if (ret != 0) {
            fprintf(stderr, "Failed to extend partition\n");
            goto exit;
        }
    } else {
        fprintf(stderr, "Unsupported partition number %zu\n", desired_partno);
        ret = -1;
        goto exit;
    }

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to write disk label\n");
        goto exit;
    }

exit:
    return ret;
}

void disk_clear_info(struct disk_info *disk)
{
    memset(disk, 0, sizeof(*disk));
}

static int disk_delete_userfs_partition(struct fdisk_context *ctx,
                                        struct part_info *pinfo)
{
    int ret = -1;

    if (!pinfo->used) {
        LOG("Partition %zu is not in use, nothing to delete\n", pinfo->partno);
        return 0;
    }

    LOG("Deleting userfs partition %zu\n", pinfo->partno);

    ret = fdisk_delete_partition(ctx, pinfo->partno);
    if (ret != 0) {
        fprintf(stderr, "Failed to delete partition %zu\n", pinfo->partno);
        return -1;
    }

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to write disk label after deletion\n");
        return -1;
    }

    // Update partition info
    memset(pinfo, 0, sizeof(*pinfo));

    LOG("Partition %zu deleted successfully\n", pinfo->partno);
    return 0;
}

int step1_create_userfs_partition(struct args *args, struct disk_info *disk)
{
    int ret                   = -1;
    uint64_t device_size      = 0;
    struct fdisk_context *ctx = NULL;
    struct fdisk_label *label = NULL;

    fdisk_init_debug(0x0);
    blkid_init_debug(0x0);

    ctx = fdisk_new_context();
    if (!ctx) {
        fprintf(stderr, "Failed to create fdisk context\n");
        goto exit;
    }

    if (fdisk_assign_device(ctx, DISK, RO_ENABLED) < 0) {
        fprintf(stderr, "Failed to assign device\n");
        goto exit;
    }

    label = fdisk_get_label(ctx, "dos");
    if (!label) {
        fprintf(stderr, "Failed to get label\n");
        goto exit;
    }

    disk->type = fdisk_label_get_type(label);
    if (disk->type != FDISK_DISKLABEL_DOS) {
        fprintf(stderr, "Unsupported partition table type\n");
        goto exit;
    }

    ret = disk_read_partitions(ctx, label, disk);
    if (ret != 0) {
        fprintf(stderr, "Failed to read disk info\n");
        goto exit;
    }

    if (disk_get_size(DISK, &device_size) != 0) {
        fprintf(stderr, "Failed to get device size\n");
        goto exit;
    }
    ASSERT(device_size == disk->total_size,
           "Device size does not match total sectors * SECTOR_SIZE");

    disk_display_info(disk);

    struct part_info *userfs_part = &disk->partitions[USERFS_PART_NO];

    // If the user asked to delete the userfs partition, do it now
    if (args->flags & FLAG_USERFS_DELETE) {
        ret = disk_delete_userfs_partition(ctx, userfs_part);
        if (ret != 0) {
            fprintf(stderr, "Failed to delete userfs partition\n");
            goto exit;
        }

        // Success - cleanup and return success
        ret = fdisk_deassign_device(ctx, 0);
        if (ret != 0) {
            fprintf(stderr, "Failed to deassign device\n");
            goto exit;
        }
        fdisk_unref_context(ctx);

        // Nothing to do after deletion, exit
        exit(EXIT_SUCCESS);
    } else {
        // otherwise try to create the userfs partition if it doesn't exist
        ret = disk_dos_create_userfs_partition(ctx, label, disk, USERFS_PART_NO);
        if (ret == 0) {
            // FIRST BOOT: Userfs partition created successfully:
            // we prefer to reformat the userfs partition to BTRFS even if it exists
            // from a previous installation, unless the user asked to trust it
            // with the -t flag.
            if (args->flags & FLAG_USERFS_TRUST_RESIDENT) {
                printf("First boot: Trusting existing userfs partition without "
                       "formatting\n");
            } else {
                printf("First boot: Userfs partition created, formatting to BTRFS\n");
                args->flags |= FLAG_USERFS_FORCE_FORMAT;
            }
        } else if (ret == 1) {
            // NOT FIRST BOOT: Userfs partition already exists:
            // we do want to keep the existing userfs partition if it exists
        } else {
            fprintf(stderr, "Failed to create userfs partition\n");
            goto exit;
        }

        // Do sync
        ret = fdisk_deassign_device(ctx, 0);
        if (ret != 0) {
            fprintf(stderr, "Failed to deassign device\n");
            goto exit;
        }
        fdisk_unref_context(ctx);
    }

    return 0;

exit:
    disk_clear_info(disk);
    if (ctx) fdisk_unref_context(ctx);
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
