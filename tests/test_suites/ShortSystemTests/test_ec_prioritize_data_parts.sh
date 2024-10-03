timeout_set 1 minute

CHUNKSERVERS=4 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	MASTER_CUSTOM_GOALS="10 ec_3_1: \$ec(3,1)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# Get the chunkservers that have parity parts for the given file
function getChunkserversWithParities() {
	local file="${1}"
	saunafs fileinfo "${file}" \
		| grep "part 4/4 of ec(3,1)" \
		| awk '{print $3}' | sort -u
}

dir="${info[mount0]}/dir"
mkdir "${dir}"
saunafs setgoal ec_3_1 "${dir}"

# Make the last chunkserver have less available space and restart it
sed -i s/"HDD_LEAVE_SPACE_DEFAULT = 128MiB"/"HDD_LEAVE_SPACE_DEFAULT = 1024MiB"/g \
       "${info[chunkserver3_cfg]}"
saunafs_chunkserver_daemon 3 stop
saunafs_chunkserver_daemon 3 start
saunafs_wait_for_all_ready_chunkservers

# Create a file using goal ec_3_1
dd if=/dev/zero of="${dir}/file" bs=1MiB count=512 &> /dev/null

# Count how many chunkservers received parity parts
chunkservers_with_parities=$(getChunkserversWithParities "${dir}/file")
echo "Chunkservers with parity parts:"
echo "${chunkservers_with_parities}"
parity_chunkservers=$(echo "${chunkservers_with_parities}" | wc -l)
echo "Parity chunkservers: ${parity_chunkservers}"

# As chunkserver 3 has less available space, it should receive all parity parts
assert_equals 1 ${parity_chunkservers}

# Disable prioritizing data parts in the master and reload it
echo "PRIORITIZE_DATA_PARTS = 0" >> "${info[master_cfg]}"
saunafs_master_daemon reload

# Give the master some time to reload
sleep 3

# Recreate the file
rm "${dir}/file"
dd if=/dev/zero of="${dir}/file" bs=1MiB count=512 &> /dev/null

# Count how many chunkservers received parity parts this time
chunkservers_with_parities=$(getChunkserversWithParities "${dir}/file")
echo "Chunkservers with parity parts:"
echo "${chunkservers_with_parities}"
parity_chunkservers=$(echo "${chunkservers_with_parities}" | wc -l)
echo "Parity chunkservers: ${parity_chunkservers}"

# Parities should be distributed among all chunkservers
assert_less_than 1 ${parity_chunkservers}
