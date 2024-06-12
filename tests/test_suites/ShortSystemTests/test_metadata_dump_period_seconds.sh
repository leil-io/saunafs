timeout_set '1 minute'
period=5
wait_period=3
master_cfg="METADATA_DUMP_PERIOD_SECONDS = ${period}"
master_cfg+="|MAGIC_DEBUG_LOG = ${TEMP_DIR}/log|LOG_FLUSH_ON=DEBUG"

CHUNKSERVERS=1 \
	MASTERSERVERS=1 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="${master_cfg}" \
	setup_local_empty_saunafs info

is_time_for_metadata_dump() {
	# The period is 5 seconds, so the modulo should be 0 every 5 seconds in UNIX time
	[ "$(($(date +%s) % period))" -eq 0 ]
}

echo "Checking metadata dump every ${period} seconds, current seconds: $(date +%S)"
# Wait for first metadata dump to be in sync with the next steps
while ! is_time_for_metadata_dump; do
	echo "Waiting for metadata dump to sync up, current seconds: $(date +%S)"
	sleep 1
done

truncate -s0 "${TEMP_DIR}/log"
# Wait a second, otherwise a race condition could happen where it skips
# waiting until the next metadata dump
sleep 1
while ! is_time_for_metadata_dump; do
	assert_awk_finds_no '/periodic metadata dump:/' "${TEMP_DIR}/log"
	echo "Waiting for first metadata dump, current seconds: $(date +%S)"
	sleep 1
done

# It's time for the metadata dump, wait a bit more for the master to finish it
echo "Time for first metadata dump, waiting ${wait_period} seconds, current seconds: $(date +%S)"
sleep ${wait_period}
echo "Looking for first metadata dump, current seconds: $(date +%S)"

# The metadata dump should be in the log now
log=$(cat "${TEMP_DIR}/log")
truncate -s0 "${TEMP_DIR}/log"
assert_awk_finds '/periodic metadata dump:/' "${log}"

# Wait for the next metadata dump
truncate -s0 "${TEMP_DIR}/log"
while ! is_time_for_metadata_dump; do
	assert_awk_finds_no '/periodic metadata dump:/' "${TEMP_DIR}/log"
	echo "Waiting for second metadata dump, current seconds: $(date +%S)"
	sleep 1
done
echo "Time for second metadata dump, waiting ${wait_period} seconds, current seconds: $(date +%S)"
sleep ${wait_period}
echo "Looking for second metadata dump, current seconds: $(date +%S)"

log=$(cat "${TEMP_DIR}/log")
truncate -s0 "${TEMP_DIR}/log"
assert_awk_finds '/periodic metadata dump:/' "${log}"
