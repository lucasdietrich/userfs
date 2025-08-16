# userfs

1. [x] Create a new partition after the last partition on the disk which occupies the whole disk.
2. [w] Format the partition as btrfs, mount it to `/mnt/userfs`.
3. [w] Create btrfs subvolumes and associated overlayfs for directories `/etc`, `/var` and `/home`.
4. [w] Create a init script to run the program after the `mountall` command.

## Remote debug:

- Run `gdbserver :1234 ./userfs`

## Tips

Wipe a partition with command `wipefs --all /dev/mmcblk0p3` (replace with your partition).
