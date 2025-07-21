/*
 * Copyright (c) 2025 Lucas Dietrich <lucas.dietrich.git@proton.me>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// clang-format off
 /*
 * Userfs Partition Management Tool
 *
 * This program manages a userfs partition on /dev/mmcblk0 and sets up
 * persistent storage with overlayfs for an embedded Linux system.
 *
 * MAIN COMMAND FLOW:
 *
 * 1. PARSE ARGUMENTS:
 *    - Parse command line options (-v verbose, -d delete, -f force format, -h help)
 *
 * 2. DISK INSPECTION & PARTITION MANAGEMENT:
 *    - Get disk size using ioctl(BLKGETSIZE64) on /dev/mmcblk0
 *    - Read existing partition table using libfdisk
 *    - Analyze current partitions (boot, rootfs, userfs)
 *
 *    IF delete flag (-d):
 *      - Delete userfs partition (partition #2) and exit
 *    ELSE:
 *      - Create userfs partition if it doesn't exist using remaining free space
 *      - Write partition table changes to disk using fdisk_write_disklabel()
 *
 * 3. PARTITION TABLE REFRESH:
 *    - Run `partprobe /dev/mmcblk0` to refresh kernel partition table
 *
 * 4. FILESYSTEM PROBING:
 *    - Probe filesystem on userfs partition (/dev/mmcblk0p3) using libblkid
 *    - Detect existing filesystem type and UUID
 *
 * 5. BTRFS FILESYSTEM CREATION:
 *    - Skip if already BTRFS and not forced (-f flag)
 *    - Run `mkfs.btrfs -f /dev/mmcblk0p3` if partition is unformatted or force flag used
 *    - Create mount point /mnt/userfs
 *    - Mount BTRFS filesystem on /mnt/userfs
 *    - Create BTRFS subvolumes:
 *      * vol-data (for /var and /home overlays)
 *      * vol-config (for /etc overlay)
 *
 * 6. OVERLAYFS SETUP:
 *    - Unmount existing /var/volatile tmpfs
 *    - For each mount point (/etc, /var, /home):
 *      * Create upper and work directories in appropriate BTRFS subvolumes
 *      * Unmount existing mount if present
 *      * Mount overlayfs with lowerdir=original, upperdir=persistent, workdir=work
 *    - Remount /var/volatile as tmpfs with mode 0755
 *
 * RESULT:
 * The program creates a persistent userfs partition with BTRFS and sets up
 * overlayfs mounts to make /etc, /var, and /home writable and persistent
 * on what appears to be an embedded Linux system with read-only root filesystem.
 *
 * This scripts can be illustrated as follows:
 * 
 *     # create a userfs partition at /dev/mmcblk0p3 using fdisk
 *  
 *     partprobe /dev/mmcblk0
 *     /usr/bin/mkfs.btrfs -f /dev/mmcblk0p3
 *  
 *     # mount userfs partition
 *     mkdir -p /mnt/userfs
 *     mount -t btrfs /dev/mmcblk0p3 /mnt/userfs
 *  
 *     # create subvolumes
 *     if ! btrfs subvolume show /mnt/userfs/vol-data >/dev/null 2>&1; then
 *        btrfs subvolume create /mnt/userfs/vol-data
 *     fi
 *     if ! btrfs subvolume show /mnt/userfs/vol-config >/dev/null 2>&1; then
 *        btrfs subvolume create /mnt/userfs/vol-config
 *     fi
 *  
 *     # if /var/volatile is already mounted, we need to unmount it first
 *     if mount | grep /var/volatile > /dev/null; then
 *        umount /var/volatile
 *     fi
 *  
 *     # create overlayfs for var, etc, and home
 *     mkdir -p /mnt/userfs/vol-config/etc /mnt/userfs/vol-config/.work.etc
 *     mount -t overlay overlay \
 *        -o lowerdir=/etc,upperdir=/mnt/userfs/vol-config/etc,workdir=/mnt/userfs/vol-config/.work.etc \
 *        /etc
 *  
 *     mkdir -p /mnt/userfs/vol-data/var /mnt/userfs/vol-data/.work.var
 *     mount -t overlay overlay \
 *        -o lowerdir=/var,upperdir=/mnt/userfs/vol-data/var,workdir=/mnt/userfs/vol-data/.work.var \
 *        /var
 *  
 *     mkdir -p /mnt/userfs/vol-data/home /mnt/userfs/vol-data/.work.home
 *     mount -t overlay overlay \
 *        -o lowerdir=/home,upperdir=/mnt/userfs/vol-data/home,workdir=/mnt/userfs/vol-data/.work.home \
 *        /home
 *  
 *     # mount /var/volatile again
 *     mount -t tmpfs tmpfs /var/volatile
 */
// clang-format on

#include "libfdisk/libfdisk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include <cstdio>
#include <errno.h>

#include <blkid.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DISK "/dev/mmcblk0"

#define USERFS_MOUNT_POINT "/mnt/userfs"

#define BOOT_PART_NO   0u
#define ROOTFS_PART_NO 1u
#define USERFS_PART_NO 2u

// MBR DOS "Linux"
#define USERFS_PART_CODE 0x83

#define RO_ENABLED 0

#define SECTOR_SIZE 512
#define KB          (1024)
#define MB          (1024 * KB)
#define GB          (1024 * MB)

#define USERFS_MIN_SIZE_B (1llu * GB)
#define USERFS_MIN_SIZE_S (USERFS_MIN_SIZE_B / SECTOR_SIZE)

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static int verbose = 0;

#define LOG(fmt, ...)                                                                    \
    do {                                                                                 \
        if (verbose) {                                                                   \
            printf(fmt, ##__VA_ARGS__);                                                  \
        }                                                                                \
    } while (0)

#define ASSERT(cond, msg)                                                                \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "Assertion failed: %s\n", msg);                              \
            return -1;                                                                   \
        }                                                                                \
    } while (0)

enum fs_type {
    FS_TYPE_UNKNOWN = 0,
    FS_TYPE_BTRFS   = 1,
    FS_TYPE_EXT4    = 2,
};

struct fs_info {
    enum fs_type type;
    char uuid[37u]; // UUID is 36 characters + null terminator
    char _reversed[3];
};

#define BTRFS_SV_DATA_INDEX   0
#define BTRFS_SV_CONFIG_INDEX 1

static const char *btrfs_subvolumes[] = {
    [BTRFS_SV_DATA_INDEX]   = "vol-data",
    [BTRFS_SV_CONFIG_INDEX] = "vol-config",
};

struct overlayfs_mount_point {
    const char *lowerdir;
    const char *upper_name;
    const char *work_name;
    const char *mount_point;
    size_t btrfs_sv_index;
};

static const struct overlayfs_mount_point overlayfs_mount_points[] = {
    {
        .lowerdir       = "/etc",
        .upper_name     = "etc",       // will end up as /mnt/userfs/vol-config/etc
        .work_name      = ".work.etc", // will end up as /mnt/userfs/vol-config/.work.etc
        .mount_point    = "/etc",
        .btrfs_sv_index = BTRFS_SV_CONFIG_INDEX,
    },
    {
        .lowerdir       = "/var",
        .upper_name     = "var",       // will end up as /mnt/userfs/vol-config/var
        .work_name      = ".work.var", // will end up as /mnt/userfs/vol-config/.work.var
        .mount_point    = "/var",
        .btrfs_sv_index = BTRFS_SV_DATA_INDEX,
    },
    {
        .lowerdir       = "/home",
        .upper_name     = "home",       // will end up as /mnt/userfs/vol-data/home
        .work_name      = ".work.home", // will end up as /mnt/userfs/vol-data/.work.home
        .mount_point    = "/home",
        .btrfs_sv_index = BTRFS_SV_DATA_INDEX,
    },
};

static const char *fs_type_to_string(enum fs_type type)
{
    switch (type) {
    case FS_TYPE_BTRFS:
        return "btrfs";
    case FS_TYPE_EXT4:
        return "ext4";
    case FS_TYPE_UNKNOWN:
    default:
        return "unknown";
    }
}

struct part_info {
    size_t index;
    fdisk_sector_t start;
    fdisk_sector_t end;
    fdisk_sector_t size;
    size_t partno;
    int used;

    struct fs_info fs_info;
};

#define MAX_PARTITIONS 4

struct disk_info {
    fdisk_sector_t total_sectors;
    size_t partition_count;
    struct part_info partitions[MAX_PARTITIONS];
    size_t free_sectors;
};

static void fs_info_display(const struct fs_info *info)
{
    if (!info) return;

    printf("Filesystem Info:\n");
    printf("  Type: %s\n", fs_type_to_string(info->type));
    printf("  UUID: %s\n", info->uuid[0] ? info->uuid : "Not set");
}

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

static int disk_read_info(struct fdisk_context *ctx, struct disk_info *disk)
{
    disk->total_sectors = fdisk_get_nsectors(ctx);

    size_t actual_partition_count = fdisk_get_npartitions(ctx);
    disk->partition_count         = (actual_partition_count > MAX_PARTITIONS)
                                        ? MAX_PARTITIONS
                                        : actual_partition_count;

    struct fdisk_partition *part = NULL;
    for (size_t n = 0; n < disk->partition_count; n++) {
        struct part_info *pinfo = &disk->partitions[n];

        pinfo->index = n;
        pinfo->start = 0;
        pinfo->end   = 0;
        pinfo->size  = 0;
        pinfo->used  = fdisk_is_partition_used(ctx, n);

        if (!pinfo->used) continue;

        if (fdisk_get_partition(ctx, n, &part) < 0) continue;

        struct fdisk_parttype *pt = fdisk_partition_get_type(part);
        if (!pt) continue;

        pinfo->start  = fdisk_partition_get_start(part);
        pinfo->end    = fdisk_partition_get_end(part);
        pinfo->size   = fdisk_partition_get_size(part);
        pinfo->partno = fdisk_partition_get_partno(part);
    }

    size_t last_used_index = 0;
    for (size_t n = 0; n < disk->partition_count; n++) {
        if (disk->partitions[n].used) last_used_index = n;
    }

    disk->free_sectors = disk->total_sectors - disk->partitions[last_used_index].end - 1;

    return 0;
}

static void disk_display_info(const struct disk_info *disk, uint64_t device_size)
{
    LOG("Device size: %lu bytes (%lu MB)\n", device_size, device_size / MB);
    LOG("\tsectors: %llu\n", (unsigned long long)disk->total_sectors);
    LOG("\tpartitions count: %zu\n", disk->partition_count);
    LOG("\tfree space at the end: %zu sectors (%zu MB)\n",
        disk->free_sectors,
        disk->free_sectors * SECTOR_SIZE / MB);

    for (size_t n = 0; n < disk->partition_count; n++) {
        const struct part_info *pinfo = &disk->partitions[n];

        if (!pinfo->used) {
            LOG("[%zu] Partition is unused\n", n);
            continue;
        }

        uint64_t approx_size_mb = pinfo->size * SECTOR_SIZE / MB;

        LOG("[%zu] partno: %lu start: %llu end: %llu size: %llu (%llu MB)\n",
            n,
            pinfo->partno,
            (unsigned long long)pinfo->start,
            (unsigned long long)pinfo->end,
            (unsigned long long)pinfo->size,
            (unsigned long long)approx_size_mb);
    }
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
 * @param pinfo The partition information structure to fill in.
 * @return 0 on success, -1 on failure, 0 if partition was created, 1 if partition already exists.
 */
static int disk_create_userfs_partition(struct fdisk_context *ctx,
                                        struct fdisk_label *label,
                                        struct disk_info *disk,
                                        struct part_info *pinfo)
{
    int ret                      = -1;
    struct fdisk_partition *part = NULL;
    struct fdisk_parttype *pt    = NULL;

    ASSERT(pinfo->index >= 1, "Partition index must be >= 1");

    if (pinfo->used) {
        fprintf(stderr, "Partition %zu is already defined\n", pinfo->index);
        return 1;
    }

    if (disk->free_sectors < USERFS_MIN_SIZE_S) {
        fprintf(stderr, "Not enough free space for userfs partition\n");
        goto exit;
    }

    pinfo->partno = pinfo->index;
    pinfo->start  = disk->total_sectors - disk->free_sectors;
    pinfo->end    = disk->total_sectors - 1;
    pinfo->size   = disk->free_sectors;
    pinfo->used   = 1;

    LOG("Creating userfs partition: start=%llu, end=%llu, size=%llu\n",
        (unsigned long long)pinfo->start,
        (unsigned long long)pinfo->end,
        (unsigned long long)pinfo->size);

    ASSERT(disk->partitions[pinfo->index - 1].end + 1 == pinfo->start,
           "Previous partition end does not match current partition start");

    ASSERT(pinfo->end - pinfo->start + 1 == pinfo->size,
           "Partition size does not match start and end");

    part = fdisk_new_partition();
    if (!part) {
        fprintf(stderr, "Failed to create new partition\n");
        goto exit;
    }

    fdisk_partition_set_partno(part, pinfo->partno);
    fdisk_partition_set_start(part, pinfo->start);
    fdisk_partition_set_size(part, pinfo->size);

    pt = fdisk_label_get_parttype_from_code(label, USERFS_PART_CODE);
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

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to write disk label\n");
        goto exit;
    }

exit:
    if (pt) fdisk_unref_parttype(pt);
    if (part) fdisk_unref_partition(part);
    return ret;
}

static void disk_free_info(struct disk_info *disk)
{
    disk->partition_count = 0;
    disk->total_sectors   = 0;
}

static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("Manage userfs partition on %s\n\n", DISK);
    printf("Options:\n");
    printf("  -d    Delete partition %u (userfs) if it exists\n", USERFS_PART_NO);
    printf("  -t    Trust existing userfs filesystem (if valid) after partition creation (first boot)\n");
    printf("  -f	Force mkfs.btrfs even if already initialized (mutually exclusive with -t)\n");
    printf("  -o    Skip overlayfs setup (useful for debugging)\n");
    printf("  -v    Enable verbose output\n");
    printf("  -h    Show this help message\n");
    printf("  (no args) Create partition %u (userfs) if it doesn't exist\n",
           USERFS_PART_NO);
    printf("\n");
}

static int disk_delete_userfs_partition(struct fdisk_context *ctx,
                                        struct part_info *pinfo)
{
    int ret = -1;

    if (!pinfo->used) {
        LOG("Partition %zu is not in use, nothing to delete\n", pinfo->index);
        return 0;
    }

    LOG("Deleting userfs partition %zu (partno %zu)\n", pinfo->index, pinfo->partno);

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

    LOG("Partition %zu deleted successfully\n", pinfo->index);
    return 0;
}

static void command_display(const char *program, char *const argv[])
{
    if (!program || !argv) return;

    printf("Running command: %s ", program);
    for (int i = 0; argv[i]; i++) {
        printf("%s ", argv[i]);
    }
    printf("\n");
}

static int command_run(char *buf, size_t *buflen, const char *program, char *const argv[])
{
    int ret       = -1;
    int pipefd[2] = {-1, -1}; // [0] = read, [1] = write
    pid_t pid;
    bool capture_output = (buf && buflen);

    if ((!program || !argv) || (buf && !buflen) || (!buf && buflen) ||
        (capture_output && *buflen == 0)) {
        errno = EINVAL;
        return -1;
    }

    if (capture_output && pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        goto cleanup;
    } else if (pid == 0) {
        // Child
        if (capture_output) {
            close(pipefd[0]); // Close read end

            if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                perror("dup2");
                _exit(EXIT_FAILURE);
            }

            close(pipefd[1]); // Not needed after dup2
        }

        execvp(program, argv);
        // If execvp returns, it failed
        perror("execvp");
        _exit(EXIT_FAILURE);
    } else {
        // Parent
        if (capture_output) {
            close(pipefd[1]); // Close write end

            ssize_t nread = read(pipefd[0], buf, *buflen);
            if (nread < 0) {
                perror("read");
                goto cleanup;
            }
            *buflen = (size_t)nread;
        }

        ret = waitpid(pid, NULL, 0);
        if (ret < 0) {
            perror("waitpid");
        }
    }

cleanup:
    if (pipefd[0] != -1) close(pipefd[0]);
    if (pipefd[1] != -1) close(pipefd[1]);
    return ret;
}

static int create_directory(const char *dir)
{
    struct stat sb;

    LOG("Creating directory: %s\n", dir);

    if (stat(dir, &sb) == 0) {
        if (S_ISDIR(sb.st_mode)) {
            LOG("Directory already exists: %s\n", dir);
            return 0;
        } else {
            fprintf(stderr, "Path exists but is not a directory: %s\n", dir);
            return -1;
        }
    }

    if (errno != ENOENT) {
        fprintf(stderr, "Failed to check directory existence: %s\n", dir);
        perror("stat");
        return -1;
    }

    // Directory does not exist, try to create it
    if (mkdir(dir, 0755) != 0) {
        fprintf(stderr, "Failed to create directory: %s\n", dir);
        perror("mkdir");
        return -1;
    }

    return 0;
}

static int fs_probe(const char *part_device, struct fs_info *info)
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

    LOG("Partition %s:\n", part_device);
    LOG("  Filesystem UUID: %s\n", fs_uuid ? fs_uuid : "Not set");
    LOG("  Type: %s\n", type ? type : "unknown");

    if (fs_uuid) {
        strncpy(info->uuid, fs_uuid, sizeof(info->uuid) - 1);
        info->uuid[sizeof(info->uuid) - 1] = '\0'; // Ensure null termination
    }

    if (type) {
        if (strcmp(type, "btrfs") == 0) {
            info->type = FS_TYPE_BTRFS;
        } else if (strcmp(type, "ext4") == 0) {
            info->type = FS_TYPE_EXT4;
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

#define FLAG_USERFS_DELETE         (1 << 1u)
#define FLAG_USERFS_FORCE_FORMAT   (1 << 2u)
#define FLAG_USERFS_TRUST_RESIDENT (1 << 3u)
#define FLAG_USERFS_SKIP_OVERLAYS  (1 << 4u)

struct args {
    uint32_t flags; // Bitmask for flags
};

static int parse_args(int argc, char *argv[], struct args *args)
{
    int opt;

    if (!args) {
        fprintf(stderr, "Invalid arguments\n");
        return -1;
    }

    while ((opt = getopt(argc, argv, "hdfvot")) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'd':
            args->flags |= FLAG_USERFS_DELETE;
            break;
        case 'f':
            args->flags |= FLAG_USERFS_FORCE_FORMAT;
            break;
        case 't':
            args->flags |= FLAG_USERFS_TRUST_RESIDENT;
            break;
        case 'o':
            args->flags |= FLAG_USERFS_SKIP_OVERLAYS;
            break;
        case 'v':
            verbose = 1;
            break;
        case '?':
            fprintf(stderr, "Unknown option: -%c\n", opt);
            print_usage(argv[0]);
            return -1;
        default:
            fprintf(stderr, "Unknown option: -%c\n", opt);
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

static int step1_create_userfs_partition(struct args *args, struct disk_info *disk)
{
    int ret                   = -1;
    uint64_t device_size      = 0;
    struct fdisk_context *ctx = NULL;
    struct fdisk_label *label = NULL;

    if (disk_get_size(DISK, &device_size) != 0) {
        fprintf(stderr, "Failed to get device size\n");
        goto exit;
    }

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

    int type = fdisk_label_get_type(label);
    if (type != FDISK_DISKLABEL_DOS) {
        fprintf(stderr, "Unsupported partition table type\n");
        goto exit;
    }

    ret = disk_read_info(ctx, disk);
    if (ret != 0) {
        fprintf(stderr, "Failed to read disk info\n");
        goto exit;
    }

    disk_display_info(disk, device_size);

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
    }

    // otherwise try to create the userfs partition if it doesn't exist
    ret = disk_create_userfs_partition(ctx, label, disk, userfs_part);
    if (ret == 0) {
        // FIRST BOOT: Userfs partition created successfully:
        // we prefer to reformat the userfs partition to BTRFS even if it exists
        // from a previous installation, unless the user asked to trust it
        // with the -t flag.
        if (args->flags & FLAG_USERFS_TRUST_RESIDENT) {
            printf("First boot: Trusting existing userfs partition without formatting\n");
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

    return 0;

exit:
    disk_free_info(disk);
    if (ctx) fdisk_unref_context(ctx);
    return ret;
}

static int disk_partprobe(const char *device)
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

static int step2_create_btrfs_filesystem(struct args *args, struct part_info *userfs_part)
{
    int ret = -1;

    // Some assertions ...
    ASSERT(userfs_part->used, "Userfs partition should be created and in use");
    ASSERT(userfs_part->partno == USERFS_PART_NO,
           "Userfs partition number should match expected value");

    // inspect the partition info after changes
    char userfs_part_device[PATH_MAX];
    snprintf(userfs_part_device,
             sizeof(userfs_part_device),
             "%sp%zu",
             DISK,
             userfs_part->partno + 1u); // TODO really + 1u ???
    LOG("Userfs partition device: %s\n", userfs_part_device);

    ret = fs_probe(userfs_part_device, &userfs_part->fs_info);
    if (ret != 0) {
        fprintf(stderr,
                "Failed to probe filesystem on %s: %s\n",
                userfs_part_device,
                strerror(errno));
        goto exit;
    }

    fs_info_display(&userfs_part->fs_info);

    bool do_create_btrfs = false;
    if (args->flags & FLAG_USERFS_FORCE_FORMAT) {
        do_create_btrfs = true;
        LOG("Userfs partition (%s) will be formatted to BTRFS due to force flag\n",
            userfs_part_device);
    }

    switch (userfs_part->fs_info.type) {
    case FS_TYPE_BTRFS:
        break;
    case FS_TYPE_EXT4:
        break;
    case FS_TYPE_UNKNOWN:
    default:
        do_create_btrfs = true;
        break;
    }

    if (do_create_btrfs) {
        // If the userfs partition is not BTRFS, create it
        LOG("Creating BTRFS filesystem on %s\n", userfs_part_device);

        const char *const mkfs_args[] = {"mkfs.btrfs",
                                        "-f", // Force creation
                                        userfs_part_device,
                                        NULL};

        command_display(mkfs_args[0], (char *const *)mkfs_args);
        ret = command_run(NULL, NULL, mkfs_args[0], (char *const *)mkfs_args);
        LOG("mkfs.btrfs returned: %d\n", ret);
        if (ret < 0) {
            fprintf(stderr, "Failed to create BTRFS filesystem: %s\n", strerror(errno));
            goto exit;
        }

        LOG("BTRFS filesystem created successfully on %s\n", userfs_part_device);

        // Create the mount point if it doesn't exist
        ret = create_directory(USERFS_MOUNT_POINT);
        if (ret != 0) {
            fprintf(stderr,
                    "Failed to create mount point %s: %s\n",
                    USERFS_MOUNT_POINT,
                    strerror(errno));
            goto exit;
        }
    }

    // Mount the btrfs filesystem
    LOG("Mounting BTRFS filesystem on %s\n", USERFS_MOUNT_POINT);

    ret = mount(userfs_part_device, USERFS_MOUNT_POINT, "btrfs", 0, NULL);
    if (ret != 0) {
        fprintf(stderr,
                "Failed to mount BTRFS filesystem on %s: %s\n",
                USERFS_MOUNT_POINT,
                strerror(errno));
        goto exit;
    }

    if (do_create_btrfs) {
        // Create subvolumes
        // FIXME use the btrfs library instead of running commands

        char sv_name[PATH_MAX];
        const char *btrfs_create_subvolumes[] = {
            "btrfs",
            "subvolume",
            "create",
            sv_name, // Placeholder for subvolume name
            NULL,    // End of arguments
        };

        for (size_t sv = 0u; sv < ARRAY_SIZE(btrfs_subvolumes); sv++) {
            snprintf(
                sv_name, sizeof(sv_name), "%s/%s", USERFS_MOUNT_POINT, btrfs_subvolumes[sv]);

            LOG("Creating BTRFS subvolume: %s\n", sv_name);

            command_display(btrfs_create_subvolumes[0],
                            (char *const *)btrfs_create_subvolumes);
            ret = command_run(NULL,
                            NULL,
                            btrfs_create_subvolumes[0],
                            (char *const *)btrfs_create_subvolumes);
            if (ret < 0) {
                fprintf(stderr,
                        "Failed to create BTRFS subvolume %s: %s\n",
                        btrfs_create_subvolumes[3],
                        strerror(errno));
                goto exit;
            }
        }
    }

    return 0;

exit:
    return ret;
}

static int step3_create_overlayfs(struct args *args)
{
    int ret;

    (void)args; // Unused for now

    // First we need to umount /var/volatile tmpfs if it is already mounted
    ret = umount2("/var/volatile", MNT_DETACH);
    if (ret < 0) {
        fprintf(stderr,
                "Failed to unmount /var/volatile: %s, continuing anyway\n",
                strerror(errno));
    }

    // Create overlayfs directories
    char upper_dir[PATH_MAX];
    char work_dir[PATH_MAX];

    for (size_t i = 0; i < ARRAY_SIZE(overlayfs_mount_points); i++) {
        const struct overlayfs_mount_point *mp = &overlayfs_mount_points[i];
        const char *btrfs_sv_name              = btrfs_subvolumes[mp->btrfs_sv_index];

        // Create upper and work directories paths
        snprintf(upper_dir,
                 sizeof(upper_dir),
                 "%s/%s/%s",
                 USERFS_MOUNT_POINT,
                 btrfs_sv_name,
                 mp->upper_name);
        snprintf(work_dir,
                 sizeof(work_dir),
                 "%s/%s/%s",
                 USERFS_MOUNT_POINT,
                 btrfs_sv_name,
                 mp->work_name);

        LOG("Creating overlayfs directories: upper=%s, work=%s\n", upper_dir, work_dir);

        // Create directories if they don't exist
        ret = create_directory(upper_dir);
        if (ret != 0) {
            fprintf(stderr,
                    "Failed to create upper directory %s: %s\n",
                    upper_dir,
                    strerror(errno));
            goto exit;
        }

        ret = create_directory(work_dir);
        if (ret != 0) {
            fprintf(stderr,
                    "Failed to create work directory %s: %s\n",
                    work_dir,
                    strerror(errno));
            goto exit;
        }

        LOG("Creating overlayfs mount point: %s\n", mp->mount_point);

        // Ensure the mount are not already mounted
        ret = umount2(mp->mount_point, MNT_DETACH);
        if (ret < 0 && errno != EINVAL) { // EINVAL means not
            // mounted, which is fine
            fprintf(stderr,
                    "Failed to unmount %s: %s, continuing anyway\n",
                    mp->mount_point,
                    strerror(errno));
        }

        // Now mount the overlayfs
        char mount_options[PATH_MAX + PATH_MAX + PATH_MAX +
                           64]; // Enough space for options
        snprintf(mount_options,
                 sizeof(mount_options),
                 "lowerdir=%s,upperdir=%s,workdir=%s",
                 mp->lowerdir,
                 upper_dir,
                 work_dir);

        printf("Mounting overlayfs on %s with options: %s\n",
               mp->mount_point,
               mount_options);

        ret = mount("overlay", mp->mount_point, "overlay", 0, mount_options);
        if (ret < 0) {
            fprintf(stderr,
                    "Failed to mount overlayfs on %s: %s\n",
                    mp->mount_point,
                    strerror(errno));
            goto exit;
        }
    }

    // Finally mount /var/volatile again
    printf("Mounting tmpfs on /var/volatile with mode 0755\n");

    ret = mount("tmpfs", "/var/volatile", "tmpfs", 0, "mode=0755");
    if (ret < 0) {
        fprintf(stderr, "Failed to mount /var/volatile: %s\n", strerror(errno));
        goto exit;
    }

    return 0;

exit:
    return ret;
}

int main(int argc, char *argv[])
{
    int ret               = -1;
    struct disk_info disk = {0};
    struct args args      = {0};

    ret = parse_args(argc, argv, &args);
    if (ret != 0) {
        fprintf(stderr, "Failed to parse arguments\n");
        goto exit;
    }

    // STEP1: Inspect the disk and create userfs partition if it doesn't exist
    ret = step1_create_userfs_partition(&args, &disk);
    if (ret != 0) {
        fprintf(stderr, "Failed to create userfs partition: %s\n", strerror(errno));
        goto exit;
    }

    struct part_info *userfs_part = &disk.partitions[USERFS_PART_NO];

    // Some assertions ...
    ASSERT(userfs_part->used, "Userfs partition should be created and in use");
    ASSERT(userfs_part->partno == USERFS_PART_NO,
           "Userfs partition number should match expected value");

    // parprob
    ret = disk_partprobe(DISK);
    if (ret < 0) {
        fprintf(stderr, "Failed to partprobe: %s\n", strerror(errno));
        goto exit;
    }

    // STEP2: Create BTRFS filesystem on the userfs partition
    ret = step2_create_btrfs_filesystem(&args, userfs_part);
    if (ret != 0) {
        fprintf(stderr, "Failed to create BTRFS filesystem: %s\n", strerror(errno));
        goto exit;
    }

    if (args.flags & FLAG_USERFS_SKIP_OVERLAYS) {
        printf("Skipping overlayfs setup as per user request\n");
        disk_free_info(&disk);
        return 0; // Nothing more to do
    }

    // STEP3: Create overlayfs for /etc, /var and /home
    ret = step3_create_overlayfs(&args);
    if (ret != 0) {
        fprintf(stderr, "Failed to create overlayfs: %s\n", strerror(errno));
        goto exit;
    }

    disk_free_info(&disk);
    return 0;

exit:
    disk_free_info(&disk);
    return ret;
}
