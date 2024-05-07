# Create an installation with 3 chunkservers, 1 disk each.
# CS 0 has a disk which will fail during the test when reading bigger files.
USE_RAMDISK=YES \
	MOUNTS=2 \
	CHUNKSERVERS=3 \
	CHUNKSERVER_EXTRA_CONFIG="HDD_TEST_FREQ = 10000" \
	CHUNKSERVER_0_DISK_0="$RAMDISK_DIR/pread_far_EIO_hdd_0" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|ACCEPTABLE_DIFFERENCE = 1.0`
			`|CHUNKS_WRITE_REP_LIMIT = 20`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 0" \
	setup_local_empty_saunafs info

# Create a directory with many files on mountpoint
cd "${info[mount0]}"
mkdir goal2
saunafs setgoal 2 goal2
FILE_SIZE=1234 file-generate goal2/small_{1..10}
FILE_SIZE=1M   file-generate goal2/big_{1..10}

# Restart the first chunkserver preloading pread with EIO-throwing version
LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libchunk_operations_eio.so" \
		assert_success saunafs_chunkserver_daemon 0 restart
saunafs_wait_for_all_ready_chunkservers

# Read small files. It should not cause a disk error.
for i in {1..10}; do
	cd ${info[mount$((i % 2))]}
	assert_success file-validate goal2/small_*
done
sleep 1
list=$(saunafs_admin_master_no_password list-disks)
assert_equals 3 "$(wc -l <<< "$list")"
assert_awk_finds_no '$4 != "no"' "$list"

# Read our big files, redefined pread is supposed to return EIO somewhere in the middle.
# Do this many times to make it more probable that the damaged disk will be used.
for i in {1..30}; do
	cd ${info[mount$((i % 2))]}
	assert_success file-validate goal2/big_*
	assert_success file-validate goal2/small_*
done

# Assert that exactly disks marked "pread_far_EIO" are marked as damaged
sleep 1
list=$(saunafs_admin_master_no_password list-disks)
assert_equals 3 "$(wc -l <<< "$list")"
assert_awk_finds_no '(/EIO/ && $4 != "yes") || (!/EIO/ && $4 != "no")' "$list"

# Assert that data is replicated to chunkservers 1, 2 and no chunk is stored on cs 0
for f in goal2/*; do
	assert_eventually_prints "" "saunafs fileinfo '$f' | grep ':${info[chunkserver0_port]}'"
	assert_eventually_prints 2 "saunafs fileinfo '$f' | grep copy | wc -l"
done
