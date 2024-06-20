timeout_set 90 seconds

# Creates a file in EC(3, 2) on all-legacy version of SaunaFS.
# Then checks if file is still readable after
#   a) updating just master
#   b) updating all services

export SAFS_MOUNT_COMMAND="sfsmount"

CHUNKSERVERS=5
	USE_RAMDISK=YES \
	START_WITH_LEGACY_SAUNAFS=YES \
	MASTERSERVERS=2 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1|OPERATIONS_DELAY_INIT = 0" \
	setup_local_empty_saunafs info

REPLICATION_TIMEOUT='30 seconds'

# Start test with master, 5 chunkservers and mount running old SaunaFS code
# Ensure that we work on legacy version
assert_equals 1 $(saunafs_old_admin_master info | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 5 $(saunafs_old_admin_master list-chunkservers | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 1 $(saunafs_old_admin_master list-mounts | grep $SAUNAFSXX_TAG | wc -l)

cd "${info[mount0]}"
mkdir dir
assert_success saunafsXX saunafs setgoal ec32 dir
cd dir

function generate_file {
	FILE_SIZE=12345678 BLOCK_SIZE=12345 file-generate $1
}

# Test if reading and writing on old SaunaFS works:
assert_success generate_file file0
assert_success file-validate file0

# Start shadow
saunafs_master_n 1 restart
assert_eventually "saunafs_shadow_synchronized 1"

# Replace old SaunaFS master with SaunaFS master:
saunafs_master_daemon restart
# Ensure that versions are switched
assert_equals 0 $(saunafs_admin_master info | grep $SAUNAFSXX_TAG | wc -l)
saunafs_wait_for_all_ready_chunkservers
# Check if files can still be read:
assert_success file-validate file0

# Replace old SaunaFS CS with SaunaFS CS and test the client upgrade:
for i in {0..4}; do
	saunafsXX_chunkserver_daemon $i stop
	saunafs_chunkserver_daemon $i start
done
saunafs_wait_for_all_ready_chunkservers

cd "$TEMP_DIR"
# Unmount old SaunaFS client:
assert_success saunafs_mount_unmount 0
# Mount SaunaFS client:
assert_success saunafs_mount_start 0
cd -
# Test if all files produced so far are readable:
assert_success file-validate file0
