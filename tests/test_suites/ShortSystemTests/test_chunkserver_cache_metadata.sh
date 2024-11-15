CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=2 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

number_of_files=100
goals="2 ec21"

for goal in ${goals}; do
	mkdir ${goal}
	saunafs setgoal ${goal} ${goal}

	echo "Writing ${number_of_files} small files with goal ${goal}"
	for i in $(seq 1 ${number_of_files}); do
		dd if=/dev/urandom of="${goal}/file${i}" bs=1K count=1 oflag=direct &> /dev/null
	done
done

drop_caches

# Stopping or restarting gracefully the chunkservers will generate the metadata
# cache files.
saunafs_chunkserver_daemon 0 restart
saunafs_chunkserver_daemon 1 restart
saunafs_chunkserver_daemon 2 restart
saunafs_wait_for_all_ready_chunkservers

for goal in ${goals}; do
	echo "Reading ${number_of_files} small files with goal ${goal}"
	for i in $(seq 1 ${number_of_files}); do
		dd if="${goal}/file${i}" of=/dev/null bs=1K count=1 &> /dev/null
	done
done
