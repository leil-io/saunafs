timeout_set 90 seconds

# A long scenario of SaunaFS upgrade from legacy to current version,
# checking if multiple mini-things work properly, in one test.
#
# TODO: Rethink this test, we shouldn't do partial upgrades like this. However,
# there may be reasons why we need do it (for example, because of the hardware
# it can be impossible to upgrade everything at once, or where it's useful to
# test a newer client against an existing cluster, perhaps for compatibility
# reasons).

export SAFS_MOUNT_COMMAND="sfsmount"

CHUNKSERVERS=2 \
	USE_RAMDISK=YES \
	MASTERSERVERS=2 \
	START_WITH_LEGACY_SAUNAFS=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_TIME = 1|OPERATIONS_DELAY_INIT = 0" \
	setup_local_empty_saunafs info

REPLICATION_TIMEOUT='30 seconds'

# Start the test with master, 2 chunkservers and mount running old SaunaFS code
# Ensure that we work on legacy version
assert_equals 1 $(saunafs_old_admin_master info | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 2 $(saunafs_old_admin_master list-chunkservers | grep $SAUNAFSXX_TAG | wc -l)
assert_equals 1 $(saunafs_old_admin_master list-mounts | grep $SAUNAFSXX_TAG | wc -l)

cd "${info[mount0]}"
mkdir dir
assert_success saunafsXX saunafs setgoal 2 dir
cd dir

function generate_file {
	FILE_SIZE=12345678 BLOCK_SIZE=12345 file-generate $1
}

# Test if reading and writing on old SaunaFS works:
assert_success generate_file file0
assert_success file-validate file0

# Start old shadow
saunafsXX_shadow_daemon_n 1 start
assert_eventually "saunafs_shadow_synchronized 1"

# Replace old SaunaFS master with newer SaunaFS master:
saunafs_master_daemon restart

# Replace shadow
saunafs_master_n 1 restart

# Ensure that versions are switched
assert_equals 0 $(saunafs_admin_master info | grep $SAUNAFSXX_TAG | wc -l)
saunafs_wait_for_all_ready_chunkservers
# Check if files can still be read:
assert_success file-validate file0
# Check if setgoal/getgoal still work:
assert_success mkdir dir
for goal in {1..9}; do
	assert_equals "dir: $goal" "$(saunafsXX saunafs setgoal "$goal" dir || echo FAILED)"
	assert_equals "dir: $goal" "$(saunafsXX saunafs getgoal dir || echo FAILED)"
	expected="dir:"$'\n'" directories with goal  $goal :          1"
	assert_equals "$expected" "$(saunafsXX saunafs getgoal -r dir || echo FAILED)"
done


# Check if replication from old SaunaFS CS (chunkserver) to SaunaFS CS works:
saunafsXX_chunkserver_daemon 1 stop
assert_success generate_file file1
assert_success file-validate file1
saunafs_chunkserver_daemon 1 start
assert_eventually \
	'[[ $(saunafsXX saunafs checkfile file1 | grep "chunks with 2 copies" | wc -l) == 1 ]]' "$REPLICATION_TIMEOUT"
saunafsXX_chunkserver_daemon 0 stop
# Check if SaunaFS CS can serve newly replicated chunks to old SaunaFS client:
assert_success file-validate file1

# Replication from SaunaFS CS to old SaunaFS CS is not guaranteed, but writes are supported
saunafsXX_chunkserver_daemon 0 start
saunafs_wait_for_all_ready_chunkservers
assert_success generate_file file2
assert_success file-validate file2

# Check if SaunaFS CS and old SaunaFS CS can communicate with each other when writing a file
# with goal = 2.
# Produce many files in order to test both chunkservers order during write:
many=5
for i in $(seq $many); do
	assert_success generate_file file3_$i
done
# Check if new files can be read both from MooseFS and from SaunaFS CS:
saunafsXX_chunkserver_daemon 0 stop
for i in $(seq $many); do
	assert_success file-validate file3_$i
done
saunafsXX_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 stop
saunafs_wait_for_ready_chunkservers 1
for i in $(seq $many); do
	assert_success file-validate file3_$i
done
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_all_ready_chunkservers

# Replace old SaunaFS CS with SaunaFS CS and test the client upgrade:
saunafsXX_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 0 start
saunafs_wait_for_ready_chunkservers 1
cd "$TEMP_DIR"
# Unmount old SaunaFS client:
assert_success saunafs_mount_unmount 0
# Mount SaunaFS client:
assert_success saunafs_mount_start 0
cd -
# Test if all files produced so far are readable:
assert_success file-validate file0
assert_success file-validate file1
assert_success file-validate file2
for i in $(seq $many); do
	assert_success file-validate file3_$i
done
