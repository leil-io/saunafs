password="good-password"
chunkservers=5
mounts=4
# Values to search for
master_config_value="NO_ATIME = 0"
mount_config_value=mfscachemode=NEVER
chunk_config_value="READ_AHEAD_KB = 1024"
metalogger_config_value="BACK_LOGS = 50"

CHUNKSERVERS=$chunkservers \
	CHUNKSERVER_EXTRA_CONFIG="${chunk_config_value}" \
	MASTER_EXTRA_CONFIG="${master_config_value}" \
	MOUNT_EXTRA_CONFIG="${mount_config_value}" \
	MOUNTS=${mounts} \
	USE_RAMDISK="YES" \
	ADMIN_PASSWORD="$password" \
	setup_local_empty_saunafs info

# Setup metalogger config with a key-value to search for
echo $metalogger_config_value >> $TEMP_DIR/saunafs/etc/sfsmetalogger.cfg
saunafs_metalogger_daemon start

master_port="${saunafs_info_[matocl]}"
shadow_port="${saunafs_info_[masterauto_matocl]}"

# Make sure that wrong password doesn't work
assert_failure saunafs-admin dump-config localhost "$master_port" <<< "wrong-password"

# Make sure that good password works
config=$(saunafs-admin dump-config localhost "$master_port" <<< "$password")
printf "${config}\n"
expect_equals 1 $(grep "${master_config_value// = /: }" <<< "${config}" | wc -l)

# Expect 5 lines with $chunk_config_value
expect_equals $chunkservers $(grep "${chunk_config_value// = /: }" <<< "$config" | wc -l)

# Expect 1 line with $metalogger_config_value
expect_equals 1 $(grep "${metalogger_config_value// = /: }" <<< "$config" | wc -l)

# Find mounts
expect_equals $mounts $(grep "${mount_config_value//=/: }" <<< "$config" | wc -l)

# Make sure that shadow works as well
# Note that shadow does not contain other service configurations, only shadow
config=$(saunafs-admin dump-config localhost "$shadow_port" <<< "$password")
printf "${config}\n"
expect_equals 1 $(grep "${master_config_value// = /: }" <<< "${config}" | wc -l)

# Test defaults dump
config=$(saunafs-admin dump-config --defaults localhost "$master_port" <<< "$password")
printf "${config}\n"
expect_equals 1 $(grep "GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS: 0.1" <<< "${config}" | wc -l)
expect_equals 5 $(grep "REPLICATION_BANDWIDTH_LIMIT_KBPS: 0" <<< "${config}" | wc -l)
