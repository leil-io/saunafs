start_proxy() {
	# Accept one connection from sfsmount on the fake port
	socat tcp-listen:$1,reuseaddr system:"
		socat stdio tcp\:$(get_ip_addr)\:$2 |  # connect to real server
		{
			dd bs=1k count=12k ;               # forward 12MB
			sleep 1d ;                         # and go catatonic
		}" &
}

if ! is_program_installed socat; then
	test_fail "Configuration error, please install 'socat'"
fi

timeout_set 1 minute

CHUNKSERVERS=4 \
	DISK_PER_CHUNKSERVER=1 \
	MOUNT_EXTRA_CONFIG="mfscachemode=NEVER" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

dir="${info[mount0]}/dir"
mkdir "$dir"
saunafs setgoal xor3 "$dir"
FILE_SIZE=123456789 file-generate "$dir/file"

# Find any chunkserver serving part 1 of some chunk
csid=$(find_first_chunkserver_with_chunks_matching 'chunk_xor_1_of_3*')
port=${info[chunkserver${csid}_port]}

# Limit data transfer from this chunkserver
start_proxy $port $((port + 1000))
saunafs_chunkserver_daemon $csid stop
LD_PRELOAD="${SAUNAFS_INSTALL_FULL_LIBDIR}/libredirect_bind.so" saunafs_chunkserver_daemon $csid start
saunafs_wait_for_all_ready_chunkservers

if ! file-validate "$dir/file"; then
	test_add_failure "Data read from file is different than written"
fi
