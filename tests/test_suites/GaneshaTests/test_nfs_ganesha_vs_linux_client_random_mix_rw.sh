#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#
# Goal of test:
# Verify the performance of Ganesha and Linux clients when performing random
# mix of reads and writes with the fio tool.
#
assert_program_installed fio

timeout_set 2 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

test_error_cleanup() {
	cd ${TEMP_DIR}
	sudo umount -l ${TEMP_DIR}/mnt/ganesha
	sudo pkill -9 ganesha.nfsd
}

mkdir -p ${TEMP_DIR}/mnt/ganesha
mkdir -p ${info[mount0]}/linux

create_ganesha_pid_file

cd ${info[mount0]}

cat <<EOF > ${info[mount0]}/ganesha.conf
NFS_KRB5 {
	Active_krb5=false;
}
NFSV4 {
	Grace_Period = 5;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 1;
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
SaunaFS {
	PNFS_DS = true;
	PNFS_MDS = true;
}
EOF

sudo /usr/bin/ganesha.nfsd -f ${info[mount0]}/ganesha.conf

check_rpc_service
sudo mount -vvvv localhost:/ $TEMP_DIR/mnt/ganesha

echo ""
echo "Running fio random mix of read and write on top of Ganesha Client:"
cd ${TEMP_DIR}/mnt/ganesha

# Run fio random mix of read and write on top of Ganesha Client
ganesha_report="$(fio --name=random_mix_rw_ganesha \
  --directory=${TEMP_DIR}/mnt/ganesha --size=500M --rw=rw --numjobs=1 \
  --ioengine=libaio --group_reporting --bs=4M --direct=1 --iodepth=4)"

# print Ganesha rw statistics
echo "${ganesha_report}"

ganesha_read_speed="$(echo "${ganesha_report}" | grep "READ: bw=" | \
                      cut -d ' ' -f 10 | cut -c2- | sed s/'),'/''/g)"
ganesha_write_speed="$(echo "${ganesha_report}" | grep "WRITE: bw=" | \
                      cut -d ' ' -f 9 | cut -c2- | sed s/'),'/''/g)"

echo ""
echo "Running fio random mix of read and write on top of Linux Client:"
cd ${info[mount0]}

# Run fio random mix of read and write on top of Linux Client
linuxClient_report="$(fio --name=name=random_mix_rw_linux \
  --directory=${info[mount0]}/linux --size=500M --rw=rw --numjobs=1 \
  --ioengine=libaio --group_reporting --bs=4M --direct=1 --iodepth=4)"

# print Linux Client rw statistics
echo "${linuxClient_report}"

linuxClient_read_speed="$(echo "${linuxClient_report}" | grep "READ: bw=" | \
                          cut -d ' ' -f 10 | cut -c2- | sed s/'),'/''/g)"
linuxClient_write_speed="$(echo "${linuxClient_report}" | grep "WRITE: bw=" | \
                          cut -d ' ' -f 9 | cut -c2- | sed s/'),'/''/g)"

# Show performances of both clients
echo ""
echo " Operation: Ganesha Client ---- Linux Client ---"
echo -e " READ:      ${ganesha_read_speed}  \t\t ${linuxClient_read_speed}"
echo -e "WRITE:      ${ganesha_write_speed} \t\t ${linuxClient_write_speed}"

test_error_cleanup || true
