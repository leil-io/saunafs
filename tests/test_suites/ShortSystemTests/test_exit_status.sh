CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

get_status() {
	"$@" >&2 | true
	echo ${PIPESTATUS[0]}
}

# Test exit statuses of the chunkserver
expect_less_or_equal 2 $(get_status sfschunkserver -c "$TEMP_DIR/nonexistent_file" start)
expect_less_or_equal 2 $(get_status saunafs_chunkserver_daemon 0 wrongusage)
expect_equals 0 $(get_status saunafs_chunkserver_daemon 0 isalive)
expect_equals 0 $(get_status saunafs_chunkserver_daemon 0 restart)
expect_equals 0 $(get_status saunafs_chunkserver_daemon 0 isalive)
expect_equals 0 $(get_status saunafs_chunkserver_daemon 0 stop)
expect_equals 0 $(get_status saunafs_chunkserver_daemon 0 stop)
expect_equals 1 $(get_status saunafs_chunkserver_daemon 0 isalive)
expect_equals 0 $(get_status saunafs_chunkserver_daemon 0 start)
expect_equals 0 $(get_status saunafs_chunkserver_daemon 0 isalive)

# Test exit statuses of the master server
expect_less_or_equal 2 $(get_status saunafs_master_daemon some_typo)
expect_less_or_equal 2 $(get_status saunafs_master_daemon -@ restart)
expect_equals 0 $(get_status saunafs_master_daemon isalive)
expect_equals 0 $(get_status saunafs_master_daemon restart)
expect_equals 0 $(get_status saunafs_master_daemon isalive)
expect_equals 0 $(get_status saunafs_master_daemon stop)
expect_equals 0 $(get_status saunafs_master_daemon stop)
expect_equals 1 $(get_status saunafs_master_daemon isalive)
mv "${info[master_data_path]}/metadata.sfs" "${info[master_data_path]}/metadata.sfs.xxx"
expect_less_or_equal 2 $(get_status saunafs_master_daemon start)
expect_less_or_equal 2 $(get_status saunafs_master_daemon restart)
expect_equals 1 $(get_status saunafs_master_daemon isalive)
mv "${info[master_data_path]}/metadata.sfs.xxx" "${info[master_data_path]}/metadata.sfs"
expect_equals 0 $(get_status saunafs_master_daemon start)
expect_equals 0 $(get_status saunafs_master_daemon isalive)
