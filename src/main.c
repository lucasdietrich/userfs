#include "libfdisk/libfdisk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define DISK "/dev/mmcblk0"

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

int disk_read_info(struct fdisk_context *ctx, struct disk_info *disk)
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

void disk_display_info(const struct disk_info *disk, uint64_t device_size)
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

int disk_create_userfs_partition(struct fdisk_context *ctx,
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

void disk_free_info(struct disk_info *disk)
{
	disk->partition_count = 0;
	disk->total_sectors	  = 0;
}

void print_usage(const char *program_name)
{
	printf("Usage: %s [OPTIONS]\n", program_name);
	printf("Manage userfs partition on %s\n\n", DISK);
	printf("Options:\n");
	printf("  -d    Delete partition %d (userfs) if it exists\n", USERFS_PART_NO);
	printf("  -v    Enable verbose output\n");
	printf("  -h    Show this help message\n");
	printf("  (no args) Create partition %d (userfs) if it doesn't exist\n",
		   USERFS_PART_NO);
	printf("\n");
}

int disk_delete_userfs_partition(struct fdisk_context *ctx, struct partition_info *pinfo)
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

int run_command(char *buf, ssize_t *buflen, const char *program, char *const argv[])
{
	int ret;

	#define R 0
	#define W 1

	int pipeds[2u]; // [read write]

	ret = pipe(pipeds);
	if (ret < 0) {
		printf("pip ret: %d\n", ret);
		exit(EXIT_FAILURE);
	}

	// char b[3] = {'a', 'b', 'c'};
	// printf("w: %ld\n", write(pipeds[1], b, 3));
	// b[0] = b[1] = b[2] = '\0';
	// printf("r: %ld %hhu %hhu %hhu\n", read(pipeds[0], b, 3), b[0], b[1], b[2]);
	
	ret = fork();

	if (ret < 0) {
		fprintf(stderr, "fork ret: %d\n", ret);
		close(pipeds[0]);
		close(pipeds[1]);
		goto exit;
	} else if (ret == 0) {
		// child
		close(pipeds[R]);
		ret = dup2(pipeds[W], STDOUT_FILENO);
		close(pipeds[W]);
		fprintf(stderr, "dup2 ret: %d\n", ret);

		printf("execvp %s\n", program);
		ret = execvp(program, argv);
		if (ret < 0) {
			printf("execvp failed ret: %d", ret);
			exit(EXIT_FAILURE);
		} else {
			// unreachable, execvp succeeded
		}
	} else {
		// parent
		int pid = ret;
		printf("child pid: %d\n", pid);

		*buflen = read(pipeds[R], buf, *buflen);
		printf("buflen: %ld\n", *buflen);
		close(pipeds[W]);

		ret = waitpid(pid, NULL, 0);
		printf("waitpid ret: %d\n", ret);
	}

	return ret;

exit:
	return ret;
}

int main(int argc, char *argv[])
{

	int ret				  = 0;

	char *const args[] = {
		"/bin/ls",
		"-lh",
		".",
		NULL
	};
	char buf[100];
	ssize_t buflen = sizeof(buf);

	ret = run_command(buf, &buflen, "/bin/ls", args);
	printf("run_command ret: %d\n", ret);

	if (buflen != 0) {
		printf("ZZZ: %s", buf);
	}

	return 0;

	uint64_t device_size  = 0;
	struct disk_info disk = {0};
	int delete_mode		  = 0;

	struct fdisk_context *ctx = NULL;
	struct fdisk_label *label = NULL;

	// Parse command line arguments
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return 0;
		} else if (strcmp(argv[i], "-d") == 0) {
			delete_mode = 1;
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			print_usage(argv[0]);
			return -1;
		}
	}

	if (disk_get_size(DISK, &device_size) != 0) ERR_GOTO("Failed to get device size");

	fdisk_init_debug(0x0);

	ctx = fdisk_new_context();
	if (!ctx) ERR_GOTO("Failed to create fdisk context");

	if (fdisk_assign_device(ctx, DISK, RO_ENABLED) < 0)
		ERR_GOTO("Failed to assign device");

	label = fdisk_get_label(ctx, "dos");
	if (!label) ERR_GOTO("Failed to get label");

	int type = fdisk_label_get_type(label);
	if (type != FDISK_DISKLABEL_DOS) ERR_GOTO("Unsupported partition table type");

	if (disk_read_info(ctx, &disk) != 0) ERR_GOTO("Failed to read disk info");

	disk_display_info(&disk, device_size);

	if (delete_mode) {
		if (disk_delete_userfs_partition(ctx, &disk.partitions[USERFS_PART_NO]) != 0)
			ERR_GOTO("Failed to delete userfs partition");
	} else {
		if (disk_create_userfs_partition(
				ctx, label, &disk, &disk.partitions[USERFS_PART_NO]) != 0)
			ERR_GOTO("Failed to create userfs partition");
	}

	disk_free_info(&disk);

	if (fdisk_deassign_device(ctx, 0) != 0) ERR_GOTO("Failed to deassign device");

	fdisk_unref_context(ctx);
	return 0;

exit:
	if (ctx) fdisk_unref_context(ctx);
	return ret;
}
