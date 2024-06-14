timeout_set "10 minutes"

CHUNKSERVERS=5 \
	USE_LOOP_DISKS=YES \
	CHUNKSERVER_LABELS="0:A|1:B|2:C|3:D|4:E" \
	MASTER_CUSTOM_GOALS="1 default: _ _" \
	CHUNKSERVER_EXTRA_CONFIG="PERFORM_FSYNC = 1|HDD_TEST_FREQ = 10000|HDD_LEAVE_SPACE_DEFAULT = 0MiB" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 10`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|CHUNKS_WRITE_REP_LIMIT = 2`
			`|CHUNKS_READ_REP_LIMIT = 2`
			`|CHUNKS_SOFT_DEL_LIMIT = 5`
			`|CHUNKS_HARD_DEL_LIMIT = 5`
			`|OPERATIONS_DELAY_INIT = 5`
			`|OPERATIONS_DELAY_DISCONNECT = 5`
			`|ACCEPTABLE_DIFFERENCE = 0.005`
			`|CHUNKS_REBALANCING_BETWEEN_LABELS = 1" \
	setup_local_empty_saunafs info

# Shut down the 'A' server leaving 'B', 'C', 'D' and 'E' running
saunafs_chunkserver_daemon 0 stop
saunafs_wait_for_ready_chunkservers 4

# Create 400 chunks on our four chunkservers. Expect about 100 chunks to be located on each server.
FILE_SIZE=1M file-generate "${info[mount0]}"/file{1..200}
assert_eventually_prints "" "saunafs_rebalancing_status | awk '\$2 < 90 || \$2 > 110'" "5 minutes"

# Add one server and expect chunks to be rebalanced to about 80 on each server.
saunafs_chunkserver_daemon 0 start
assert_eventually_prints "" "saunafs_rebalancing_status | awk '\$2 < 70 || \$2 > 90'" "5 minutes"

# Check the chunkservers load after 5 seconds to see if it is stable.
sleep 5
assert_awk_finds_no '$2 < 70 || $2 > 90' "$(saunafs_rebalancing_status)"
