CHUNKSERVERS=3 \
	DISK_PER_CHUNKSERVER=2 \
	USE_RAMDISK=YES \
	MASTER_CUSTOM_GOALS="10 ec21: \$ec(2,1)" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

mkdir ec21
saunafs setgoal ec21 ec21
cd ec21

number_of_files=100

echo "Writing ${number_of_files} small files"
for i in $(seq 1 ${number_of_files}); do
	dd if=/dev/urandom of=file${i} bs=1K count=1 oflag=direct &> /dev/null
done

drop_caches

saunafs_chunkserver_daemon 0 restart
saunafs_chunkserver_daemon 1 restart
saunafs_chunkserver_daemon 2 restart
saunafs_wait_for_all_ready_chunkservers

echo "Reading the ${number_of_files} small files"
for i in $(seq 1 ${number_of_files}); do
	dd if=file${i} of=/dev/null bs=1K count=1 &> /dev/null
done
