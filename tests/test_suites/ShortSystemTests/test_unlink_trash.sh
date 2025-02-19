# shellcheck shell=bash
chunks_replicated_count() {
	saunafs-admin chunks-health \
		--porcelain \
		--replication \
		localhost "${info[matocl]}" | awk '$3!=0{sum+=$3}END{print sum}'
}

timeout_set 4 minutes

CHUNKSERVERS=4 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_EXTRA_CONFIG="CHUNKS_LOOP_MIN_TIME = 1`
			`|CHUNKS_LOOP_MAX_CPU = 90`
			`|OPERATIONS_DELAY_INIT = 1" \
	CHUNKSERVER_EXTRA_CONFIG="CHUNK_TRASH_ENABLED=1`
			`|CHUNK_TRASH_EXPIRATION_SECONDS=50`
			`|CHUNK_TRASH_FREE_SPACE_THRESHOLD_GB=0" \
	USE_RAMDISK="YES" \
	setup_local_empty_saunafs info

# Create a file consisting of a couple of chunks and remove it
file="${info[mount0]}/file"
xorfile="${info[mount0]}/xorfile"
touch "${file}" "${xorfile}"
saunafs setgoal 3 "${file}"
saunafs setgoal xor3 "${xorfile}"
test_file_size_mb=130
dd if=/dev/zero of="${file}" bs=1MiB count=130
dd if=/dev/zero of="${xorfile}" bs=1MiB count=130
saunafs settrashtime 0 "${file}" "${xorfile}"

# Count chunks before file removal
chunks_count_before_files_removal="$(find_all_chunks | wc -l)"
echo "Chunks count before files removal: ${chunks_count_before_files_removal}"
trash_chunks_count_before_files_removal="$(find_all_trashed_chunks | wc -l)"
echo "Trash chunks count before files removal: ${trash_chunks_count_before_files_removal}"

# Remove the files
rm -f "${file}" "${xorfile}"

# Wait for all chunks to be trashed
waiting_timeout="3 minutes"
if ! wait_for '[[ $(find_all_chunks | wc -l) == 0 ]]' "${waiting_timeout}"; then
	test_add_failure $'The following chunks were not removed:\n'"$(find_all_chunks)"
fi

# Verify the trashed chunks count
chunks_count_after_files_removal=$(find_all_chunks | wc -l)
echo "Chunks count after files removal: ${chunks_count_after_files_removal}"
trashed_chunks_count=$(find_all_trashed_chunks | wc -l)
echo "Trashed chunks count: ${trashed_chunks_count}"

MESSAGE="The trashed chunks count is not as expected"
assert_success [ "${trashed_chunks_count}" -ge "${chunks_count_before_files_removal}" ]

# Wait for trashed chunks to expire
sleep 60

# Ensure trashed chunks are removed after expiration
if [ "$(find_all_trashed_chunks | wc -l)" -ne 0 ]; then
	test_add_failure $'The trashed chunks were not removed after the trashing time'
fi
