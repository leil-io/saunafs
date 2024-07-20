timeout_set "1 minute"

# Start an installation with:
#   two servers labeled 'ssd' and 2 unlabeled servers
#   the default goal "ssd _"
USE_RAMDISK=YES \
	CHUNKSERVERS=4 \
	CHUNKSERVER_LABELS="0,1:ssd" \
	MASTER_CUSTOM_GOALS="1 default: ssd _" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|ACCEPTABLE_DIFFERENCE = 1.0`
			`|CHUNKS_WRITE_REP_LIMIT = 5`
			`|OPERATIONS_DELAY_INIT = 0`
			`|OPERATIONS_DELAY_DISCONNECT = 15" \
	setup_local_empty_saunafs info

# Leave only one unlabeled and one 'ssd' server
saunafs_chunkserver_daemon 1 stop
saunafs_chunkserver_daemon 3 stop
saunafs_wait_for_ready_chunkservers 2

# Wait few seconds to avoid unwanted replication delay.
sleep 17

# Create 20 files. Expect that for each file there are 2 chunk copies.
FILE_SIZE=1K file-generate "${info[mount0]}"/file{1..20}
assert_equals 20 $(saunafs checkfile "${info[mount0]}"/* | grep 'with 2 copies:' | wc -l)

# Stop 'ssd' server and start unlabeled server.
# We have only two unlabeled servers now.
saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 3 start
saunafs_wait_for_ready_chunkservers 2

# All chunks has 1 missing replica on 'ssd' server
# but they never should be replicated to some random server.
assert_equals 20 $(saunafs checkfile "${info[mount0]}"/* | grep 'with 1 copy:' | wc -l)
sleep 7
assert_equals 20 $(saunafs checkfile "${info[mount0]}"/* | grep 'with 1 copy:' | wc -l)

# Restart 'ssd' server.
saunafs_chunkserver_daemon 1 start
saunafs_wait_for_ready_chunkservers 3

# Replication should start immediately
assert_eventually_prints 20 'find_chunkserver_chunks 1 | wc -l' "4 seconds"

# Replication loop is no longer atomic, so we need some time to ensure
# that chunk loop tested all chunks.
sleep 2

assert_equals 20 $(saunafs checkfile "${info[mount0]}"/* | grep 'with 2 copies:' | wc -l)
