#include "disk.h"
#include "dm.h"
#include "userfs.h"
#include "utils.h"

#include <string.h>

#include <fcntl.h>
#include <libdevmapper.h>
#include <linux/limits.h>
#include <unistd.h>

#define SB_SECTORS  8
#define SECTOR_SIZE 512

#define SB_PROVIDED_DATA_SECTORS_OFFSET 16u
#define SB_PROVIDED_DATA_SECTORS_SIZE   8u

// struct superblock {
//     uint8_t magic[8];
//     uint8_t version;
//     uint8_t log2_interleave_sectors;
//     uint16_t integrity_tag_size;
//     uint32_t journal_sections;
//     uint64_t provided_data_sectors; /* userspace uses this value */
//     uint32_t flags;
//     uint8_t log2_sectors_per_block;
//     uint8_t log2_blocks_per_bitmap_bit;
//     uint8_t pad[2];
//     uint64_t recalc_sector;
//     uint8_t pad2[8];
//     uint8_t salt[SALT_SIZE];
// };

int dmsetup_create(const char *mapper_name, uint64_t sectors, const char *dev)
{
    int ret;
    char table[256];
    /* 0: start sector
     * 1/n: number of sectors (1/n sector for the superblock)
     * integrity: target type
     * dev: underlying device
     * 0: reserved sector at the beginning of the device
     * -: integrity tag size
     * J: journaled writes
     * 2: 2 additional arguments
     * internal_hash:crc32: integrity tag type
     * recalculate: recalculate the integrity tag on the fly (only if internal and
     * non-crypto hash)
     */
    // snprintf(table, sizeof(table), "0 %llu integrity %s 0 - J 1
    // internal_hash:hmac(sha256):0123456789abcdef", 1llu, part->path);
    snprintf(table,
             sizeof(table),
             "0 %llu integrity %s 0 - J 2 internal_hash:crc32 recalculate",
             sectors,
             dev);

    const char *args[] = {
        "/usr/sbin/dmsetup",
        "create",
        mapper_name,
        "--table",
        table,
        NULL,
    };

    ret = command_run(NULL, NULL, args[0], args);
    if (ret != 0) {
        ERR("Failed to create dm-integrity mapper %s\n", mapper_name);
        return ret;
    }

    return ret;
}

int dmsetup_remove(const char *mapper_name)
{
    int ret;
    const char *args[] = {
        "/usr/sbin/dmsetup",
        "remove",
        mapper_name,
        NULL,
    };

    ret = command_run(NULL, NULL, args[0], args);
    if (ret != 0) {
        ERR("Failed to remove dm-integrity mapper %s\n", mapper_name);
        return ret;
    }

    return 0;
}

int dm_sb_zeroize(const char *device)
{
    int ret, fd;

    fd = open(device, O_WRONLY);
    if (fd < 0)
        return fd;

    char sb[SB_SECTORS * SECTOR_SIZE];
    memset(sb, 0, sizeof(sb));

    ret = write(fd, sb, sizeof(sb));
    if (ret < 0)
        goto exit;

    ret = 0;
exit:
    if (fd >= 0)
        close(fd);
    return ret;
}

/* dm-integrity (doc): https://docs.kernel.org/admin-guide/device-mapper/dm-integrity.html
 * dmsetup (doc): https://man7.org/linux/man-pages/man8/dmsetup.8.html
 * dmsetup (arch): https://man.archlinux.org/man/dmsetup.8.en
 * claude: https://claude.ai/chat/a693ae12-de13-472c-8492-efbdbd3263e4
 */
int dm_integrity_map(const char *mapper_name,
                     const char *device,
                     struct block_device *dev,
                     bool reset_integrity_sb)
{
    // 1. overwrite the superblock with zeroes
    int ret, fd;

    if (reset_integrity_sb) {
        ret = dm_sb_zeroize(device);
        if (ret != 0)
            return ret;

        // 2. load the dm-integrity target with one-sector size, the kernel driver will
        // format the device
        ret = dm_create(mapper_name, 1llu, device, false);
        if (ret != 0)
            return ret;

        // 3. unload the dm-integrity target
        ret = dm_remove(mapper_name);
        if (ret != 0)
            return ret;
    }

    // 3. read the “provided_data_sectors” value from the superblock
    fd = open(device, O_RDONLY);
    if (fd < 0)
        return fd;

    char sb[SB_SECTORS * SECTOR_SIZE];
    ret = read(fd, sb, sizeof(sb));
    if (ret != sizeof(sb))
        goto exit;

    dev->sectors = 0;
    memcpy(&dev->sectors,
           sb + SB_PROVIDED_DATA_SECTORS_OFFSET,
           SB_PROVIDED_DATA_SECTORS_SIZE);

    close(fd);
    fd = -1;

    // load the dm-integrity target with the target size “provided_data_sectors”
    ret = dm_create(mapper_name, dev->sectors, device, true);
    if (ret != 0)
        goto exit;

    snprintf(dev->path, sizeof(dev->path), "/dev/mapper/%s", mapper_name);

exit:
    if (fd >= 0)
        close(fd);

    return ret;
}

int dm_create(const char *mapper_name, uint64_t sectors, const char *dev, bool wait)
{
    struct dm_task *dmt = dm_task_create(DM_DEVICE_CREATE);
    if (!dmt)
        return -1;

    char params[256];
    snprintf(params, sizeof(params), "%s 0 - J 2 internal_hash:crc32 recalculate", dev);

    dm_task_set_name(dmt, mapper_name);
    dm_task_add_target(dmt, 0, sectors, "integrity", params);
    dm_task_set_add_node(dmt, DM_ADD_NODE_ON_CREATE);

    uint32_t cookie     = 0;
    uint16_t udev_flags = 0; /* or DM_UDEV_DISABLE_LIBRARY_FALLBACK etc. */

    if (!dm_task_set_cookie(dmt, &cookie, udev_flags)) {
        dm_task_destroy(dmt);
        return -1;
    }

    int ret = dm_task_run(dmt) ? 0 : -1;

    /* Block until udev actually creates /dev/mapper/<name> */
    if (wait)
        dm_udev_wait(cookie);

    dm_task_destroy(dmt);
    return ret;
}

int udev_settle(void)
{
    const char *args[] = {"/usr/bin/udevadm", "settle", NULL};
    int ret            = command_run(NULL, NULL, args[0], args);
    if (ret != 0)
        ERR("udevadm settle failed (ret=%d)\n", ret);
    return ret;
}

int dm_remove(const char *mapper_name)
{
    struct dm_task *dmt = dm_task_create(DM_DEVICE_REMOVE);
    if (!dmt)
        return -1;

    dm_task_set_name(dmt, mapper_name);

    int ret = dm_task_run(dmt) ? 0 : -1;
    dm_task_destroy(dmt);
    return ret;
}

int dm_integrity_status_get(const char *dev_name, struct block_device *dev)
{
    struct dm_task *dmt;
    uint64_t start, length;
    char *target_type;
    char *params;
    void *next = NULL;
    int ret    = -1;

    dmt = dm_task_create(DM_DEVICE_STATUS);
    if (!dmt) {
        fprintf(stderr, "dm_task_create failed\n");
        return -1;
    }

    if (!dm_task_set_name(dmt, dev_name)) {
        fprintf(stderr, "dm_task_set_name failed\n");
        goto out;
    }

    /* ask the kernel to fill in extended info like open count */
    dm_task_no_open_count(dmt);

    if (!dm_task_run(dmt)) {
        fprintf(stderr, "dm_task_run failed (device may not exist)\n");
        goto out;
    }

    /* an integrity device normally has a single target */
    next = dm_get_next_target(dmt, next, &start, &length, &target_type, &params);

    if (!target_type) {
        LOG("[ dm-integrity %s ] no target found\n", dev_name);
        goto out;
    }

    if (strcmp(target_type, "integrity") != 0) {
        fprintf(stderr, "target type is '%s', not 'integrity'\n", target_type);
        goto out;
    }

    LOG("[ dm-integrity %s ] start: %llu length: %llu target_type: %s params: %s\n",
        dev_name,
        (unsigned long long)start,
        (unsigned long long)length,
        target_type,
        params);

    dev->sectors = length;
    snprintf(dev->path, sizeof(dev->path), "/dev/mapper/%s", dev_name);

    ret = 0;

out:
    dm_task_destroy(dmt);
    return ret;
}