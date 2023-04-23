USE_RAMDISK=YES \
	MASTERSERVERS=2 \
	MASTER_EXTRA_CONFIG="MAGIC_DISABLE_METADATA_DUMPS = 1" \
	setup_local_empty_saunafs info

# Generate some changes in the changelog
touch "${info[mount0]}/file"{1..5}

# Verify if metarestore fails (metadata is locked by the running master server)
assert_failure sfsmetarestore -a -d "${info[master_data_path]}"

# Kill the master server and retry -- should succeed; recreate the master server then
assert_success saunafs_master_daemon kill
assert_success sfsmetarestore -a -d "${info[master_data_path]}"
assert_success saunafs_master_daemon start

# Modify shadow master to use the same data dir as the primary master server
assert_success saunafs_master_n 1 stop
old_data_path=$(grep DATA_PATH "${saunafs_info_[master1_cfg]}")
grep DATA_PATH "${saunafs_info_[master0_cfg]}" >> "${saunafs_info_[master1_cfg]}"

# This should fail -- two master servers can't share data path
assert_failure saunafs_master_n 1 start

# Fix data path and try starting the shadow master again -- should work!
echo "$old_data_path" >> "${saunafs_info_[master1_cfg]}"
assert_success saunafs_master_n 1 start
