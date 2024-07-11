timeout_set 1 minute

CHUNKSERVERS=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd "${TEMP_DIR}"
	sudo umount -l "${TEMP_DIR}/mnt/ganesha"
	sudo pkill -9 ganesha.nfsd
}

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
NFS_KRB5 {
	Active_krb5=false;
}
NFSV4 {
	Grace_Period = 5;
	Lease_Lifetime = 10;
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
		io_retries = 30;
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

mkdir -p "${TEMP_DIR}/mnt/ganesha"
sudo mount -vvvv localhost:/ "${TEMP_DIR}/mnt/ganesha"

# Create a file for testing with checksum
head -c 3G /dev/random | pv > "${TEMP_DIR}/test_file"

# Upload the file using NFS while killing the master every ten seconds
while true; do echo "Restarting master daemon"; sleep 10 && saunafs_master_daemon restart; done &

pv "${TEMP_DIR}/test_file" > "${TEMP_DIR}/mnt/ganesha/test_file"
saunafs_master_daemon restart

# Get checksums for both files
original_file_checksum=$(sha256sum "${TEMP_DIR}/test_file" | awk '{print $1}')
uploaded_file_checksum=$(sha256sum "${TEMP_DIR}/mnt/ganesha/test_file" | awk '{print $1}')

# Verify both checksums are the same
assert_equals "${original_file_checksum}" "${uploaded_file_checksum}"

# Get file size in bytes
file_size=$(stat -c %s "${TEMP_DIR}/mnt/ganesha/test_file")

# Verify file size is 3GiB
assert_equals $file_size $((3 * 1024 * 1024 * 1024))

test_error_cleanup || true
