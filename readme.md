# userfs

1. [x] Create a new partition after the last partition on the disk which occupies the whole disk.
2. [ ] Format the partition as btrfs, mount it to `/mnt/userfs`.
3. [ ] Create btrfs subvolumes and associated overlayfs for directories `/etc`, `/var` and `/home`.
4. [ ] Create a init script to run the program after the `mountall` command.

## Remote debug:

- Run `gdbserver :1234 ./userfs`