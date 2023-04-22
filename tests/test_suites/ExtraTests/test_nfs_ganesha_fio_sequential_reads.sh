#
# To run this test you need to add the following lines to /etc/sudoers.d/lizardfstest:
#
# lizardfstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# lizardfstest ALL = NOPASSWD: /tmp/LizardFS-autotests/mnt/mfs0/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#
# Goal of test:
# Verify that fio tool performs correctly sequential reads on top of the Ganesha client.
#

assert_program_installed fio

timeout_set 5 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="mfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_lizardfs info

MINIMUM_PARALLEL_JOBS=4
MAXIMUM_PARALLEL_JOBS=16
PARALLEL_JOBS=$(get_nproc_clamped_between ${MINIMUM_PARALLEL_JOBS} ${MAXIMUM_PARALLEL_JOBS})

test_error_cleanup() {
	cd ${TEMP_DIR}
	sudo umount -l ${TEMP_DIR}/mnt/ganesha
	sudo pkill -9 ganesha.nfsd
}

mkdir -p ${TEMP_DIR}/mnt/ganesha

# Create PID file for Ganesha
PID_FILE=/var/run/ganesha/ganesha.pid
if [ ! -f ${PID_FILE} ]; then
	echo "ganesha.pid doesn't exists, creating it...";
	sudo mkdir -p /var/run/ganesha;
	sudo touch ${PID_FILE};
else
	echo "ganesha.pid already exists";
fi

cd ${info[mount0]}

# Copy Ganesha and libntirpc source code
cp -R "$SOURCE_DIR"/external/nfs-ganesha-4.3 nfs-ganesha-4.3
cp -R "$SOURCE_DIR"/external/ntirpc-4.3 ntirpc-4.3

# Remove original libntirpc folder to create a soft link
rm -R nfs-ganesha-4.3/src/libntirpc
ln -s ../../ntirpc-4.3 nfs-ganesha-4.3/src/libntirpc

# Create build folder to compile Ganesha
mkdir nfs-ganesha-4.3/src/build
cd nfs-ganesha-4.3/src/build

# flag -DUSE_GSS=NO disables the use of Kerberos library when compiling Ganesha
CC="ccache gcc" cmake -DCMAKE_INSTALL_PREFIX=${info[mount0]} -DUSE_GSS=NO -D_USE_FSAL_LIZARDFS=OFF ..
make -j${PARALLEL_JOBS} install

# Copy LizardFS FSAL
fsal_lizardfs=${LIZARDFS_ROOT}/lib/ganesha/libfsallizardfs.so
assert_file_exists "$fsal_lizardfs"
cp ${fsal_lizardfs} ${info[mount0]}/lib/ganesha

cat <<EOF > ${info[mount0]}/etc/ganesha/ganesha.conf
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
		Name = LizardFS;
		hostname = localhost;
		port = ${lizardfs_info_[matocl]};
		# How often to retry to connect
		io_retries = 5;
		cache_expiration_time_ms = 2500;
	}
	Protocols = 4;
	CLIENT {
		Clients = localhost;
	}
}
LizardFS {
	PNFS_DS = true;
	PNFS_MDS = true;
}
EOF

sudo ${info[mount0]}/bin/ganesha.nfsd -f ${info[mount0]}/etc/ganesha/ganesha.conf

sleep 10
sudo mount -vvvv localhost:/ $TEMP_DIR/mnt/ganesha

echo ""
echo "Running fio sequential read on top of Ganesha Client:"
cd ${TEMP_DIR}/mnt/ganesha

# Run fio sequential read test on top of Ganesha Client
fio --name=fiotest_seq_read_QD4 --directory=${TEMP_DIR}/mnt/ganesha --size=200M --rw=read --numjobs=5 --ioengine=libaio --group_reporting --bs=4M --direct=1 --iodepth=4

echo ""
echo "List of created files at the Ganesha Client:"
ls ${TEMP_DIR}/mnt/ganesha/ -lh

test_error_cleanup || true
