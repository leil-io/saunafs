CHUNKSERVERS=0 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="AUTO_RECOVERY = 1" \
	MOUNTS=0 \
	setup_local_empty_saunafs info

# Generate empty metadata file by stopping the master server and generate a long changelog.
saunafs_master_daemon stop
assert_equals 1 $(metadata_get_version "${info[master_data_path]}/metadata.sfs")
generate_changelog > "${info[master_data_path]}/changelog.sfs"

# Start the master server in background and wait until master starts to apply
# the changelog. This process will then last for a couple of seconds.
saunafs_master_daemon start &
assert_eventually 'test -e "${info[master_data_path]}/metadata.sfs.lock"'
sleep 1

# Try restoring metadata. This should fail, because master holds the lock.
expect_failure sfsmetarestore -a -d "${info[master_data_path]}"

# Expect that the master server still applies the changelog.
expect_failure saunafs-admin info localhost "${info[matocl]}"
