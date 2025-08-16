#include <stdio.h>

#include <errno.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "userfs.h"
#include "btrfs.h"
#include "utils.h"

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
#if defined(USERFS_OVERLAY_OPT)
    {
        .lowerdir       = "/opt",
        .upper_name     = "opt",       // will end up as /mnt/userfs/vol-data/opt
        .work_name      = ".work.opt", // will end up as /mnt/userfs/vol-data/.work.opt
        .mount_point    = "/opt",
        .btrfs_sv_index = BTRFS_SV_DATA_INDEX,
    },
#endif /* USERFS_OVERLAY_OPT */
};

int step3_create_overlayfs(struct args *args)
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
        const char *btrfs_sv_name              = btrfs_get_volume(mp->btrfs_sv_index);

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
