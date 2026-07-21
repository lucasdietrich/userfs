// Steps to create a swap file:

// # 1. Create empty file and disable CoW (must be done before writing data)
// touch /mnt/userfs/swapfile
// chattr +C /mnt/userfs/swapfile

// # 2. Fill with real zeros (no truncate — it creates holes that swapon rejects)
// dd if=/dev/zero of=/mnt/userfs/swapfile bs=1M count=256 status=progress

// # 3. Fix permissions and format
// chmod 0600 /mnt/userfs/swapfile
// mkswap /mnt/userfs/swapfile

// # 4. Enable
// swapon /mnt/userfs/swapfile

#include "utils.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

// unused
int verbose = 0;

// First argument is swap file path, second argument is size in MB
// Optional -f flag forces rewriting zeros even if the file already has the correct size
int main(int argc, char *argv[])
{
    int ret    = -1;
    int fd     = -1;
    bool force = false;

    int opt;
    while ((opt = getopt(argc, argv, "f")) != -1) {
        if (opt == 'f')
            force = true;
        else {
            fprintf(stderr, "Usage: %s [-f] <swapfile_path> <size_mb>\n", argv[0]);
            return 1;
        }
    }

    if (optind + 2 != argc) {
        fprintf(stderr, "Usage: %s [-f] <swapfile_path> <size_mb>\n", argv[0]);
        return 1;
    }

    const char *path = argv[optind];
    char *endptr;
    long size_mb = strtol(argv[optind + 1], &endptr, 10);
    if (*endptr != '\0' || size_mb <= 0) {
        fprintf(stderr, "Invalid size: %s\n", argv[optind + 1]);
        return 1;
    }

    off_t expected_size = (off_t)size_mb * MB;

    // Check if the file already exists with the correct size
    struct stat st;
    bool need_fill = force || stat(path, &st) != 0 || st.st_size != expected_size;

    if (need_fill) {
        // Step 1: Create empty file and disable CoW (must be done before writing data)
        fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) {
            fprintf(stderr, "Failed to create swap file %s: %s\n", path, strerror(errno));
            goto exit;
        }

        int flags = 0;
        if (ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0) {
            fprintf(stderr, "Failed to get flags on %s: %s\n", path, strerror(errno));
            goto exit;
        }
        flags |= FS_NOCOW_FL;
        if (ioctl(fd, FS_IOC_SETFLAGS, &flags) < 0) {
            fprintf(
                stderr, "Failed to set NOCOW flag on %s: %s\n", path, strerror(errno));
            goto exit;
        }

        // Step 2: Allocate real zero-filled blocks (single syscall, no userspace I/O)
        if (fallocate(fd, 0, 0, expected_size) < 0) {
            if (errno == EOPNOTSUPP) {
                // Fallback for filesystems that don't support fallocate (e.g. FAT, NFS)
                static const char zeros[MB];
                for (long i = 0; i < size_mb; i++) {
                    ssize_t written = write(fd, zeros, sizeof(zeros));
                    if (written != (ssize_t)sizeof(zeros)) {
                        fprintf(stderr,
                                "Failed to write zeros to %s: %s\n",
                                path,
                                strerror(errno));
                        goto exit;
                    }
                }
            } else {
                fprintf(stderr, "fallocate failed on %s: %s\n", path, strerror(errno));
                goto exit;
            }
        }

        // Step 3: Flush to disk before mkswap writes its header
        if (fsync(fd) < 0) {
            fprintf(stderr, "fsync failed on %s: %s\n", path, strerror(errno));
            goto exit;
        }

        close(fd);
        fd = -1;
    }

    // Step 4: Format
    const char *mkswap_args[] = {"/sbin/mkswap", path, NULL};
    ret                       = command_run(NULL, NULL, mkswap_args[0], mkswap_args);
    if (ret < 0) {
        fprintf(stderr, "Failed to format swap file %s: %s\n", path, strerror(errno));
        goto exit;
    }

    ret = 0;

exit:
    if (fd >= 0)
        close(fd);
    return ret < 0 ? 1 : 0;
}