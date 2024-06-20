timeout_set 1 minutes

USE_RAMDISK=YES \
	CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=3 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

firstDisk=$(head -1 "${info[chunkserver0_hdd]}")
isZoned=$(is_zoned_device "${firstDisk}")

# Add # to the first hdd line
if $isZoned; then
	sed -i '1s@zonefs@#zonefs@' "${info[chunkserver0_hdd]}"
else
	sed -i '1s@/mnt@#/mnt@' "${info[chunkserver0_hdd]}"
fi

saunafs_chunkserver_daemon 0 reload

# Check the disk was removed correctly
assert_eventually_prints 2 "saunafs-admin list-disks \
	--porcelain localhost ${info[matocl]} | wc -l" "30 seconds"
