#include "libfdisk/libfdisk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// #include <cstdio>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <getopt.h> 
#include <unistd.h>
#include <errno.h>

#include <blkid.h>

#define DISK		"/dev/mmcblk0"
#define PART_BOOT	DISK "p1"
#define PART_ROOTFS	DISK "p2"
#define PART_USERFS	DISK "p3"

#define BOOT_PART_NO	 0u
#define ROOTFS_PART_NO	 1u
#define USERFS_PART_NO	 2u
#define USERFS_PART_CODE 0x83

#define RO_ENABLED 0

#define SECTOR_SIZE 512
#define KB			(1024)
#define MB			(1024 * KB)
#define GB			(1024 * MB)

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

#define ERR_GOTO(msg)                                                                    \
	do {                                                                                 \
		fprintf(stderr, "%s\n", msg);                                                    \
		ret = -1;                                                                        \
		goto exit;                                                                       \
	} while (0)

struct partition_info {
	size_t index;
	fdisk_sector_t start;
	fdisk_sector_t end;
	fdisk_sector_t size;
	size_t partno;
	int used;
};

#define MAX_PARTITIONS 4

struct disk_info {
	fdisk_sector_t total_sectors;
	size_t partition_count;
	struct partition_info partitions[MAX_PARTITIONS];
	size_t free_sectors;
};

int disk_get_size(const char *device, uint64_t *size)
{
	int ret = 0;
	int fd	= -1;

	fd = open(device, O_RDWR);
	if (fd < 0) {
		perror("open");
		ERR_GOTO("Failed to open device");
	}

	if (ioctl(fd, BLKGETSIZE64, size) == -1) {
		perror("ioctl BLKGETSIZE64");
		ERR_GOTO("Failed to get device size");
	}

	if (close(fd) < 0) {
		perror("close");
		ERR_GOTO("Failed to close device");
	}

	return 0;
exit:
	if (fd >= 0) close(fd);
	return ret;
}

static int disk_part_probe(const char *device)
{
	int ret = 0;
	int fd	= -1;

	fd = open(device, 0);
	if (fd < 0) {
		perror("open");
		ERR_GOTO("Failed to open device");
	}

	if (ioctl(fd, BLKRRPART, NULL) == -1) {
		perror("ioctl BLKRRPART");
		// ERR_GOTO("Failed to probe partitions");
	}

	if (close(fd) < 0) {
		perror("close");
		ERR_GOTO("Failed to close device");
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
	disk->partition_count		  = (actual_partition_count > MAX_PARTITIONS)
										? MAX_PARTITIONS
										: actual_partition_count;

	struct fdisk_partition *part = NULL;
	for (size_t n = 0; n < disk->partition_count; n++) {
		struct partition_info *pinfo = &disk->partitions[n];

		pinfo->index = n;
		pinfo->start = 0;
		pinfo->end	 = 0;
		pinfo->size	 = 0;
		pinfo->used	 = fdisk_is_partition_used(ctx, n);

		if (!pinfo->used) continue;

		if (fdisk_get_partition(ctx, n, &part) < 0) continue;

		struct fdisk_parttype *pt = fdisk_partition_get_type(part);
		if (!pt) continue;

		pinfo->start  = fdisk_partition_get_start(part);
		pinfo->end	  = fdisk_partition_get_end(part);
		pinfo->size	  = fdisk_partition_get_size(part);
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
		const struct partition_info *pinfo = &disk->partitions[n];

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
								 struct partition_info *pinfo)
{
	int ret						 = 0;
	struct fdisk_partition *part = NULL;
	struct fdisk_parttype *pt	 = NULL;

	ASSERT(pinfo->index >= 1, "Partition index must be >= 1");

	if (pinfo->used) {
		fprintf(stderr, "Partition %zu is already defined\n", pinfo->index);
		return 0;
	}

	if (disk->free_sectors < USERFS_MIN_SIZE_S)
		ERR_GOTO("Not enough free space for userfs partition");

	pinfo->partno = pinfo->index;
	pinfo->start  = disk->total_sectors - disk->free_sectors;
	pinfo->end	  = disk->total_sectors - 1;
	pinfo->size	  = disk->free_sectors;
	pinfo->used	  = 1;

	LOG("Creating userfs partition: start=%llu, end=%llu, size=%llu\n",
		(unsigned long long)pinfo->start,
		(unsigned long long)pinfo->end,
		(unsigned long long)pinfo->size);

	ASSERT(disk->partitions[pinfo->index - 1].end + 1 == pinfo->start,
		   "Previous partition end does not match current partition start");

	ASSERT(pinfo->end - pinfo->start + 1 == pinfo->size,
		   "Partition size does not match start and end");

	part = fdisk_new_partition();
	if (!part) ERR_GOTO("Failed to create new partition");

	fdisk_partition_set_partno(part, pinfo->partno);
	fdisk_partition_set_start(part, pinfo->start);
	fdisk_partition_set_size(part, pinfo->size);

	pt = fdisk_label_get_parttype_from_code(label, USERFS_PART_CODE);
	if (!pt) ERR_GOTO("Failed to get partition type");

	fdisk_partition_set_type(part, pt);

	size_t cur_partno = -1;
	ret				  = fdisk_add_partition(ctx, part, &cur_partno);
	if (ret != 0) ERR_GOTO("Failed to add partition");

	ret = fdisk_write_disklabel(ctx);
	if (ret != 0) ERR_GOTO("Failed to write disk label");

exit:
	if (pt) fdisk_unref_parttype(pt);
	if (part) fdisk_unref_partition(part);
	return ret;
}

static void disk_free_info(struct disk_info *disk)
{
	disk->partition_count = 0;
	disk->total_sectors	  = 0;
}

static void print_usage(const char *program_name)
{
	printf("Usage: %s [OPTIONS]\n", program_name);
	printf("Manage userfs partition on %s\n\n", DISK);
	printf("Options:\n");
	printf("  -d    Delete partition %d (userfs) if it exists\n", USERFS_PART_NO);
	printf("  -f	Force mkfs.btrfs even if already initialized\n");
	printf("  -v    Enable verbose output\n");
	printf("  -h    Show this help message\n");
	printf("  (no args) Create partition %d (userfs) if it doesn't exist\n",
		   USERFS_PART_NO);
	printf("\n");
}

static int disk_delete_userfs_partition(struct fdisk_context *ctx, struct partition_info *pinfo)
{
	int ret = 0;

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
	pinfo->used	  = 0;
	pinfo->start  = 0;
	pinfo->end	  = 0;
	pinfo->size	  = 0;
	pinfo->partno = 0;

	LOG("Partition %zu deleted successfully\n", pinfo->index);
	return 0;
}

static int run_command(char *buf, ssize_t *buflen, const char *program, char *const argv[])
{
	int ret		  = -1;
	int pipefd[2] = {-1, -1}; // [0] = read, [1] = write
	pid_t pid;

	if (!buf || !buflen || *buflen <= 0 || !program || !argv) {
		errno = EINVAL;
		return -1;
	}

	if (pipe(pipefd) < 0) {
		perror("pipe");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		goto cleanup;
	} else if (pid == 0) {
		// Child
		close(pipefd[0]); // Close read end

		if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
			perror("dup2");
			_exit(EXIT_FAILURE);
		}

		close(pipefd[1]); // Not needed after dup2

		execvp(program, argv);
		// If execvp returns, it failed
		perror("execvp");
		_exit(EXIT_FAILURE);
	} else {
		// Parent
		close(pipefd[1]); // Close write end

		ssize_t nread = read(pipefd[0], buf, *buflen);
		if (nread < 0) {
			perror("read");
			goto cleanup;
		}

		*buflen = nread;
		ret		= waitpid(pid, NULL, 0);
		if (ret < 0) {
			perror("waitpid");
		}
	}

cleanup:
	if (pipefd[0] != -1) close(pipefd[0]);
	if (pipefd[1] != -1) close(pipefd[1]);
	return ret;
}

#define FLAG_USERFS_DELETE		 (1 << 1u)
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

static int do_blkid(const char *device)
{
	int ret;
	int disk_fd;

	printf("Running blkid to check partition type...\n");

	blkid_probe pr = blkid_new_probe ();
	if (!pr) {
		fprintf(stderr, "Failed to create blkid probe\n");
		return -1;
	}

	disk_fd = open(device, O_RDWR);
	if (disk_fd < 0) {
		perror("open");
		ERR_GOTO("Failed to open device");
	}

	
	// blkid_probe pr = blkid_new_probe_from_filename(DISK);

	// offset = 0, size = 0 (whole device)
	ret = blkid_probe_set_device(pr, disk_fd, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to set device for blkid probe: %s\n", strerror(errno));
		goto exit;
	}

	blkid_probe_enable_partitions(pr, true);
	blkid_probe_enable_superblocks(pr, true);
	blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_UUID | BLKID_SUBLKS_LABEL);

	ret = blkid_do_safeprobe(pr);
	printf("blkid_do_safeprobe returned: %d\n", ret);

	// ret = blkid_do_fullprobe(pr);
	// printf("blkid_do_fullprobe returned: %d\n", ret);

	blkid_partlist ls;
	int nparts;

	ls = blkid_probe_get_partitions(pr);
	nparts = blkid_partlist_numof_partitions(ls);

	printf("Number of partitions found: %d\n", nparts);

			const char *fs_uuid = NULL;
		blkid_probe_lookup_value(pr, "UUID", &fs_uuid, NULL);
		printf("Filesystem UUID: %s\n", fs_uuid);

	// iterate over all partitions and print their info
	for (int i = 0; i < nparts; i++) {
		blkid_partition part = blkid_partlist_get_partition(ls, i);
		if (part) {

			int partno = blkid_partition_get_partno(part);
			char partdev[PATH_MAX];
			snprintf(partdev, sizeof(partdev), "%sp%d", device, partno);  // e.g. /dev/mmcblk0p1

			// Open a new probe on the partition device
			blkid_probe part_probe = blkid_new_probe_from_filename(partdev);
			if (!part_probe) {
				fprintf(stderr, "Failed to create probe for %s\n", partdev);
				continue;
			}

			blkid_do_safeprobe(part_probe);

			const char *fs_uuid = NULL;
			const char *part_uuid = NULL;
			const char *type = NULL;
			blkid_probe_lookup_value(part_probe, "UUID", &fs_uuid, NULL);
			blkid_probe_lookup_value(part_probe, "PARTUUID", &part_uuid, NULL);
			blkid_probe_lookup_value(part_probe, "TYPE", &type, NULL);

			printf("Partition %d: %s\n", i, partdev);
			printf("  PARTUUID: %s\n", part_uuid);
			printf("  Filesystem UUID: %s\n", fs_uuid);
			printf("  Type: %s\n", type ? type : "unknown");

			const char *block_name = blkid_partition_get_name(part);
			const char *block_uuid = blkid_partition_get_uuid(part);
			int block_type = blkid_partition_get_type(part);
			const char *block_type_string = blkid_partition_get_type_string(part);
			blkid_loff_t start = blkid_partition_get_start(part);
			blkid_loff_t size = blkid_partition_get_size(part);

			printf("Partition %d: %s\n", i, block_name);
			printf("  UUID: %s\n", block_uuid);
			printf("  Type: %d (%s)\n", block_type, block_type_string);
			printf("  Start: %lld\n", start);
			printf("  Size: %lld\n", size);
		}
	}

exit:
	if (disk_fd >= 0) close(disk_fd);
	if (pr) blkid_free_probe(pr);
	return ret;
}

int main(int argc, char *argv[])
{
	int ret				  = 0;
	// char *const ls_args[] = {
	// 	"/bin/ls",
	// 	"-lh",
	// 	".",
	// 	NULL
	// };
	// char buf[100];
	// ssize_t buflen = sizeof(buf);

	// ret = run_command(buf, &buflen, "/bin/ls", ls_args);
	// printf("run_command ret: %d\n", ret);

	// if (buflen != 0) {
	// 	printf("ZZZ: %s", buf);
	// }

	// return 0;

	uint64_t device_size  = 0;
	struct disk_info disk = {0};
	struct args args = {0};

	struct fdisk_context *ctx = NULL;
	struct fdisk_label *label = NULL;

	if (parse_args(argc, argv, &args) != 0) {
		fprintf(stderr, "Failed to parse arguments\n");
		return -1;
	}

	if (disk_get_size(DISK, &device_size) != 0) ERR_GOTO("Failed to get device size");

	do_blkid(DISK);

	// // Re-probe partitions to ensure changes are recognized
	// LOG("Probing %s partitions...\n", DISK);
	// ret = disk_part_probe(DISK);
	// if (ret != 0) {
	// 	ERR_GOTO("Failed to probe partitions after changes");
	// }


	fdisk_init_debug(0x0);
	blkid_init_debug(0xFFFF);

	ctx = fdisk_new_context();
	if (!ctx) ERR_GOTO("Failed to create fdisk context");

	if (fdisk_assign_device(ctx, DISK, RO_ENABLED) < 0)
		ERR_GOTO("Failed to assign device");

	label = fdisk_get_label(ctx, "dos");
	if (!label) ERR_GOTO("Failed to get label");

	int type = fdisk_label_get_type(label);
	if (type != FDISK_DISKLABEL_DOS) ERR_GOTO("Unsupported partition table type");

	if (disk_read_info(ctx, &disk) != 0) {
		ERR_GOTO("Failed to read disk info");
		return -1;
	}

	disk_display_info(&disk, device_size);

	if (args.flags & FLAG_USERFS_DELETE) {
		if (disk_delete_userfs_partition(ctx, &disk.partitions[USERFS_PART_NO]) != 0)
			ERR_GOTO("Failed to delete userfs partition");

		LOG("Userfs (%d) partition deleted successfully, exiting\n", USERFS_PART_NO);
		exit(EXIT_SUCCESS);
	} else {
		if (disk_create_userfs_partition(
				ctx, label, &disk, &disk.partitions[USERFS_PART_NO]) != 0)
			ERR_GOTO("Failed to create userfs partition");
	}

	disk_free_info(&disk);

	if (fdisk_deassign_device(ctx, 0) != 0) ERR_GOTO("Failed to deassign device");

	fdisk_unref_context(ctx);


	// Run blkid to check the partition type if it already exists
	do_blkid(DISK);
	
	// sleep(1); // Give some time for the system to process changes

	// // Re-probe partitions to ensure changes are recognized
	// LOG("Re-probing %s partitions after changes...\n", DISK);
	// ret = disk_part_probe(DISK);
	// if (ret != 0) {
	// 	ERR_GOTO("Failed to probe partitions after changes");
	// }

	return 0;

exit:
	if (ctx) fdisk_unref_context(ctx);
	return ret;
}
