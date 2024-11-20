CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=2 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

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

function getMetadataCachePathForChunkserver() {
	local cs=$1
	echo $(grep "METADATA_CACHE_PATH" "${info[chunkserver${cs}_cfg]}" |
	       awk '{print $3}')
}

cd "${info[mount0]}"

generateFiles

drop_caches

# Stopping or restarting gracefully the chunkservers will generate the metadata
# cache files.
saunafs_chunkserver_daemon 0 restart
saunafs_chunkserver_daemon 1 restart
saunafs_chunkserver_daemon 2 restart
saunafs_wait_for_all_ready_chunkservers

validateFiles

# Stop the chunkservers to overwrite the metadata cache files with the generator
saunafs_chunkserver_daemon 0 stop
saunafs_chunkserver_daemon 1 stop
saunafs_chunkserver_daemon 2 stop

# Wait for the chunkservers to stop
sleep 3

echo "Overwriting metadata cache files with the generator"
for cs in $(seq 0 2); do
	metadata_cache_path=$(getMetadataCachePathForChunkserver ${cs})
	sfs-chunk-metadata-cache-generator \
		--hdd-file "${info[chunkserver${cs}_hdd]}" \
		--cache-dir "${metadata_cache_path}" \
		--syslog
done

drop_caches

# Start the chunkservers, they will scan from the metadata cache files
saunafs_chunkserver_daemon 0 start
saunafs_chunkserver_daemon 1 start
saunafs_chunkserver_daemon 2 start
saunafs_wait_for_all_ready_chunkservers

echo "Reading the files again after dropping caches"
validateFiles
