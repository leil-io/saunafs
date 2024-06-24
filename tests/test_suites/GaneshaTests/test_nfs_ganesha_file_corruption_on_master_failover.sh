#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#

timeout_set 2 minutes

CHUNKSERVERS=3 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd "${TEMP_DIR}"
	sudo umount -l "${TEMP_DIR}/mnt/ganesha"
	sudo pkill -9 ganesha.nfsd
}

# Function to get the checksum of a given file
get_checksum() {
	sha256sum $1 | awk '{ print $1 }'
}

mkdir -p "${TEMP_DIR}/mnt/ganesha"

# Create PID file for Ganesha
PID_FILE=/var/run/ganesha/ganesha.pid
if [ ! -f ${PID_FILE} ]; then
	echo "ganesha.pid doesn't exists, creating it...";
	sudo mkdir -p /var/run/ganesha;
	sudo touch "${PID_FILE}";
else
	echo "ganesha.pid already exists";
fi

cd "${info[mount0]}"

cat <<EOF > "${info[mount0]}/ganesha.conf"
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 5;
}
EXPORT {
	Attr_Expiration_Time = 0;
	Export_Id = 99;
	Path = /;
	Pseudo = /;
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
		# How often to retry to connect
		io_retries = 5;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
EOF

sudo /usr/bin/ganesha.nfsd -f "${info[mount0]}/ganesha.conf"
assert_eventually 'showmount -e localhost'

sudo mount -vvvv localhost:/ "${TEMP_DIR}/mnt/ganesha"

# Create a file for testing with checksum
head -c 3G /dev/random | tee "${TEMP_DIR}/test_file" > /dev/null

# Restart master server after 15 seconds
(
	sleep 15
	assert_success saunafs_master_daemon restart
) &

# Wait for Grace period so NFS Ganesha server will be ready
sleep 5

# Try to copy the file after master restart
while true; do
	cp "${TEMP_DIR}/test_file" "$TEMP_DIR/mnt/ganesha/test_file" && break
	echo "Unable to copy test_file through NFS, retrying in 5 seconds..."
	sleep 5
done

# Get checksums
checksum1=$(get_checksum "${TEMP_DIR}/test_file")

# To get the checksum of the file from NFS could require several retries in case
# NFS mount will not be available after restarting master server
while true; do
	checksum2=$(get_checksum "${TEMP_DIR}/mnt/ganesha/test_file")
	if [ "${checksum2}" != "" ]; then
		break
	fi
	echo "Unable to get checksums through NFS, retrying in 5 seconds..."
	sleep 5
done

checksum3=$(get_checksum "${info[mount0]}/test_file")

# Print checksums
echo "Checksum of original file: ${checksum1}"
echo "Checksum of file from NFS mount: ${checksum2}"
echo "Checksum of file from SaunaFS mount: ${checksum3}"

# Verify checksums
assert_equals "${checksum1}" "${checksum2}"
assert_equals "${checksum1}" "${checksum3}"

test_error_cleanup || true
