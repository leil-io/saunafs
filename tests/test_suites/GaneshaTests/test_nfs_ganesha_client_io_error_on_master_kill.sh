#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#

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
LOG {
	COMPONENTS {
		NFS_V4=FULL_DEBUG;
		FSAL=FULL_DEBUG;
	}
}
EOF

sudo /usr/bin/ganesha.nfsd -f "${info[mount0]}/ganesha.conf" -L /tmp/ganesha.log
assert_eventually 'showmount -e localhost'

mkdir -p "${TEMP_DIR}/mnt/ganesha"
sudo mount -vvvv localhost:/ "${TEMP_DIR}/mnt/ganesha"

# Create a file for testing with checksum
head -c 3G /dev/random | pv > "${TEMP_DIR}/test_file"

# Upload the file using NFS while killing the master every ten seconds
while true; do echo "Restarting master daemon"; sleep 10 && saunafs_master_daemon restart; done &

pv "${TEMP_DIR}/test_file" > "${TEMP_DIR}/mnt/ganesha/test_file"
saunafs_master_daemon restart

# Verify checksums
sha256sum "${TEMP_DIR}/test_file" "${TEMP_DIR}/mnt/ganesha/test_file"

# Verify file size
ls -lh "${TEMP_DIR}/mnt/ganesha/test_file"

test_error_cleanup || true
