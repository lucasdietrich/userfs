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
#include <sys/wait.h>
#include <unistd.h>

#define DISK "/dev/mmcblk0"

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
        fprintf(stderr, "Failed to open device");
        goto exit;
    }

    ret = ioctl(fd, BLKGETSIZE64, size);
    if (ret < 0) {
        perror("ioctl BLKGETSIZE64");
        fprintf(stderr, "Failed to get device size");
        goto exit;
    }

    ret = close(fd);
    if (ret < 0) {
        perror("close");
        fprintf(stderr, "Failed to close device");
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
        return 0;
    }

    if (disk->free_sectors < USERFS_MIN_SIZE_S) {
        fprintf(stderr, "Not enough free space for userfs partition");
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
        fprintf(stderr, "Failed to create new partition");
        goto exit;
    }

    fdisk_partition_set_partno(part, pinfo->partno);
    fdisk_partition_set_start(part, pinfo->start);
    fdisk_partition_set_size(part, pinfo->size);

    pt = fdisk_label_get_parttype_from_code(label, USERFS_PART_CODE);
    if (!pt) {
        fprintf(stderr, "Failed to get partition type");
        goto exit;
    }

    fdisk_partition_set_type(part, pt);

    size_t cur_partno = (size_t)-1;
    ret               = fdisk_add_partition(ctx, part, &cur_partno);
    if (ret != 0) {
        fprintf(stderr, "Failed to add partition");
        goto exit;
    }

    ret = fdisk_write_disklabel(ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to write disk label");
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
    printf("  -f	Force mkfs.btrfs even if already initialized\n");
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

static int fs_probe(const char *part_device, struct fs_info *info)
{
    int ret        = -1;
    int fd         = -1;
    blkid_probe pr = NULL;

    if (!part_device || !info) {
        fprintf(stderr, "Invalid arguments for fs_probe");
        goto exit;
    }

    // Clear the fs_info structure
    memset(info, 0, sizeof(struct fs_info));

    pr = blkid_new_probe();
    if (!pr) {
        fprintf(stderr, "Failed to create blkid probe");
        goto exit;
    }

    fd = open(part_device, 0); // Read-only mode
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "Failed to open partition device");
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
        fprintf(stderr, "Failed to close partition device");
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

#define FLAG_USERFS_DELETE       (1 << 1u)
#define FLAG_USERFS_FORCE_FORMAT (1 << 2u)

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

    while ((opt = getopt(argc, argv, "hdfv")) != -1) {
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

static int step0_create_userfs_partition(struct args *args, struct disk_info *disk)
{
    int ret                   = -1;
    uint64_t device_size      = 0;
    struct fdisk_context *ctx = NULL;
    struct fdisk_label *label = NULL;

    if (disk_get_size(DISK, &device_size) != 0) {
        fprintf(stderr, "Failed to get device size");
        goto exit;
    }

    fdisk_init_debug(0x0);
    blkid_init_debug(0x0);

    ctx = fdisk_new_context();
    if (!ctx) {
        fprintf(stderr, "Failed to create fdisk context");
        goto exit;
    }

    if (fdisk_assign_device(ctx, DISK, RO_ENABLED) < 0) {
        fprintf(stderr, "Failed to assign device");
        goto exit;
    }

    label = fdisk_get_label(ctx, "dos");
    if (!label) {
        fprintf(stderr, "Failed to get label");
        goto exit;
    }

    int type = fdisk_label_get_type(label);
    if (type != FDISK_DISKLABEL_DOS) {
        fprintf(stderr, "Unsupported partition table type");
        goto exit;
    }

    ret = disk_read_info(ctx, disk);
    if (ret != 0) {
        fprintf(stderr, "Failed to read disk info");
        goto exit;
    }

    disk_display_info(disk, device_size);

    struct part_info *userfs_part = &disk->partitions[USERFS_PART_NO];

    // If the user asked to delete the userfs partition, do it now
    if (args->flags & FLAG_USERFS_DELETE) {
        ret = disk_delete_userfs_partition(ctx, userfs_part);
        if (ret != 0) {
            fprintf(stderr, "Failed to delete userfs partition");
            goto exit;
        }

        // Success - cleanup and return success
        ret = fdisk_deassign_device(ctx, 0);
        if (ret != 0) {
            fprintf(stderr, "Failed to deassign device");
            goto exit;
        }
        fdisk_unref_context(ctx);

        // Nothing to do after deletion, exit
        exit(EXIT_SUCCESS);
    }

    // otherwise try to create the userfs partition if it doesn't exist
    ret = disk_create_userfs_partition(ctx, label, disk, userfs_part);
    if (ret != 0) {
        fprintf(stderr, "Failed to create userfs partition");
        goto exit;
    }

    // Do sync
    ret = fdisk_deassign_device(ctx, 0);
    if (ret != 0) {
        fprintf(stderr, "Failed to deassign device");
        goto exit;
    }
    fdisk_unref_context(ctx);

    return 0;

exit:
    disk_free_info(disk);
    if (ctx) fdisk_unref_context(ctx);
    return ret;
}

static int step1_create_btrfs_filesystem(struct args *args, struct part_info *userfs_part)
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

    // If the userfs partition is not BTRFS, create it
    if (do_create_btrfs) {
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
    }

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
        fprintf(stderr, "Failed to parse arguments");
        goto exit;
    }

    // STEP0: Inspect the disk and create userfs partition if it doesn't exist
    ret = step0_create_userfs_partition(&args, &disk);
    if (ret != 0) {
        fprintf(stderr, "Failed to create userfs partition: %s\n", strerror(errno));
        goto exit;
    }

    struct part_info *userfs_part = &disk.partitions[USERFS_PART_NO];

    // Some assertions ...
    ASSERT(userfs_part->used, "Userfs partition should be created and in use");
    ASSERT(userfs_part->partno == USERFS_PART_NO,
           "Userfs partition number should match expected value");

    // STEP1: Create BTRFS filesystem on the userfs partition
    ret = step1_create_btrfs_filesystem(&args, userfs_part);
    if (ret != 0) {
        fprintf(stderr, "Failed to create BTRFS filesystem: %s\n", strerror(errno));
        goto exit;
    }

    disk_free_info(&disk);
    return 0;

exit:
    disk_free_info(&disk);
    return ret;
}
