timeout_set 3 hours

CHUNKSERVERS=3 \
	MOUNTS=2 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

size=40G

# Write using one mount
cd "${info[mount0]}"
FILE_SIZE=$size file-generate file

# Read using another mount
cd "${info[mount1]}"
if ! file-validate file; then
	test_add_failure "Corrupted file of size $size"
fi
