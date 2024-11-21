CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=2 \
	USE_RAMDISK=YES \
	CHUNKSERVER_EXTRA_CONFIG="MAGIC_DEBUG_LOG = $TEMP_DIR/log|LOG_FLUSH_ON=INFO" \
	AUTO_SHADOW_MASTER="NO" \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

lastChunkserver=2
number_of_files=100
goals="2 ec21"

function generateFiles() {
	for goal in ${goals}; do
		mkdir ${goal}
		saunafs setgoal ${goal} ${goal}

		echo "Writing ${number_of_files} small files with goal ${goal}"
		for i in $(seq 1 ${number_of_files}); do
			FILE_SIZE=1024 assert_success file-generate "${goal}/file${i}"
		done
	done
}

function validateFiles() {
	for goal in ${goals}; do
		echo "Validating ${number_of_files} small files with goal ${goal}"
		for i in $(seq 1 ${number_of_files}); do
			assert_success file-validate "${goal}/file${i}"
		done
	done
}

cd "${info[mount0]}"

generateFiles

# Disable stat on chunk files at scan time
for cs in $(seq 0 ${lastChunkserver}); do
	echo "STAT_CHUNKS_AT_DISK_SCAN = 0" >> "${info[chunkserver${cs}_cfg]}"
	saunafs_chunkserver_daemon ${cs} restart
done
saunafs_wait_for_all_ready_chunkservers

# Check that all chunkserver have the stat disabled
assert_equals 3 $(grep -i "STAT_CHUNKS_AT_DISK_SCAN = 0" "${TEMP_DIR}/log" | wc -l)

drop_caches

validateFiles
