timeout_set 45 seconds

CHUNKSERVERS=4 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="10 ec_3_1: \$ec(3,1)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

dir="${info[mount0]}/dir"
mkdir "$dir"
saunafs setgoal ec_3_1 "$dir"
FILE_SIZE=6M file-generate "$dir/file"

# Corrupt data in part 1 of the chunk
csid=$(find_first_chunkserver_with_chunks_matching \
	"chunk_ec2_1_of_3*${chunk_metadata_extension}")
hdd=$(get_metadata_path "${info[chunkserver${csid}_hdd]}")
chunk=$(find "$hdd" -name "chunk_ec2_1_of_3_*${chunk_metadata_extension}")
echo aaaa | dd of="$chunk" bs=1 count=4 seek=6k conv=notrunc

if ! file-validate "$dir/file"; then
	test_add_failure "Data read from file is different than written"
fi
