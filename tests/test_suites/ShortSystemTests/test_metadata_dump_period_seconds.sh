timeout_set '1 minute'
period=5
master_cfg="METADATA_DUMP_PERIOD_SECONDS = ${period}"
master_cfg+="|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|LOG_FLUSH_ON=DEBUG"

CHUNKSERVERS=1 \
	MASTERSERVERS=1 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	setup_local_empty_saunafs info

is_time_for_metadata_dump() {
	# The period is 5 seconds, so the modulo should be 0 every 5 seconds
	[ "$(($(date +%s) % period))" -eq 0 ]
}

truncate -s0 "${TEMP_DIR}/log"
while ! is_time_for_metadata_dump; do
	assert_awk_finds_no '/periodic metadata dump:/' "${TEMP_DIR}/log"
	sleep 1
done
# It's time for the metadata dump, wait a bit more for the master to finish it
sleep 2

# The metadata dump should be in the log now
log=$(cat "${TEMP_DIR}/log")
truncate -s0 "${TEMP_DIR}/log"
assert_awk_finds '/periodic metadata dump:/' "${log}"

# Wait for the next metadata dump
truncate -s0 "${TEMP_DIR}/log"
while ! is_time_for_metadata_dump; do
	assert_awk_finds_no '/periodic metadata dump:/' "${TEMP_DIR}/log"
	sleep 1
done
sleep 2

log=$(cat "${TEMP_DIR}/log")
truncate -s0 "${TEMP_DIR}/log"
assert_awk_finds '/periodic metadata dump:/' "${log}"
