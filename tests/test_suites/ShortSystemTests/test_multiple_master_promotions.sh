timeout_set '2 minutes'

metaservers_nr=5
MASTERSERVERS=$metaservers_nr \
	USE_RAMDISK="YES" \
	CHUNKSERVER_EXTRA_CONFIG="MASTER_RECONNECTION_DELAY = 1" \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

# Start shadow masters
for ((shadow_id=1 ; shadow_id<metaservers_nr; ++shadow_id)); do
	saunafs_master_n $shadow_id start
	assert_eventually "saunafs_shadow_synchronized $shadow_id"
done

for ((loop_nr=0 ; loop_nr<$((2 * metaservers_nr)); ++loop_nr)); do
	touch "${info[mount0]}"/"master=$loop_nr"
	metadata=$(metadata_print "${info[mount0]}")

	prev_master_id=$((loop_nr % metaservers_nr))
	new_master_id=$(((loop_nr + 1) % metaservers_nr))

	# Kill the previous master
	assert_eventually "saunafs_shadow_synchronized $new_master_id"
	saunafs_master_daemon kill
	saunafs_make_conf_for_shadow $prev_master_id

	# Promote a next master
	saunafs_make_conf_for_master $new_master_id
	saunafs_master_daemon reload

	# Demote previous master to shadow
	saunafs_make_conf_for_shadow $prev_master_id
	saunafs_master_n $prev_master_id start
	assert_eventually "saunafs_shadow_synchronized $prev_master_id"

	saunafs_wait_for_all_ready_chunkservers

	MESSAGE="Data mismatch after switching masters $prev_master_id -> $new_master_id" \
			assert_no_diff "$metadata" "$(metadata_print "${info[mount0]}")"
done
