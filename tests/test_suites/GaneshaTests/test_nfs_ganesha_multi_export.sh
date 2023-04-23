#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#

timeout_set 2 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

test_error_cleanup() {
	for x in 1 2 97 99; do
		sudo umount -l $TEMP_DIR/mnt/nfs$x
	done
	sudo pkill -9 ganesha.nfsd
}

cd ${info[mount0]}

# Create mountpoints for testing
mkdir $TEMP_DIR/mnt/nfs{1,2,97,99}
mkdir ganesha

# Create PID file for Ganesha
PID_FILE=/var/run/ganesha/ganesha.pid
if [ ! -f ${PID_FILE} ]; then
	echo "ganesha.pid doesn't exists, creating it...";
	sudo mkdir -p /var/run/ganesha;
	sudo touch ${PID_FILE};
else
	echo "ganesha.pid already exists";
fi

cat <<EOF > ${info[mount0]}/ganesha.conf
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 1;
	Path = /export1;
	Pseudo = /e1;
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 3, 4;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 2;
	Path = /export2;
	Pseudo = /e2;
	Access_Type = RW;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 3, 4;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 97;
	Path = /;
	Pseudo = /e97;
	Access_Type = MDONLY;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 4;
}
EXPORT
{
	Attr_Expiration_Time = 0;
	Export_Id = 99;
	Path = /;
	Pseudo = /e99;
	Access_Type = RO;
	FSAL {
		Name = SaunaFS;
		hostname = localhost;
		port = ${saunafs_info_[matocl]};
	}
	Protocols = 4;
}
SaunaFS {
	PNFS_DS = true;
	PNFS_MDS = true;
}
NFSV4 {
	Grace_Period = 5;
}
EOF

mkdir ${info[mount0]}/export{1,2}

touch ${info[mount0]}/export1/test1
touch ${info[mount0]}/export2/test2

sudo /usr/bin/ganesha.nfsd -f ${info[mount0]}/ganesha.conf
assert_eventually 'showmount -e localhost'

for x in 1 2 99; do
	sudo mount -o v4.1 localhost:/e$x $TEMP_DIR/mnt/nfs$x
done
sudo mount -o nfsvers=4 localhost:/e97 $TEMP_DIR/mnt/nfs97

find $TEMP_DIR/mnt/nfs1 * | grep test1
assert_empty "$(find $TEMP_DIR/mnt/nfs1 | grep test2 | cat)"
find $TEMP_DIR/mnt/nfs2 * | grep test2
assert_empty "$(find $TEMP_DIR/mnt/nfs2 | grep test1 | cat)"

ls -l $TEMP_DIR/mnt/nfs1
ls -l $TEMP_DIR/mnt/nfs2
ls -l $TEMP_DIR/mnt/nfs97
ls -l $TEMP_DIR/mnt/nfs99

FILE_SIZE=1234567 file-generate $TEMP_DIR/mnt/nfs1/test1.bin
FILE_SIZE=2345678 file-generate $TEMP_DIR/mnt/nfs2/test2.bin

file-validate $TEMP_DIR/mnt/nfs1/test1.bin
file-validate $TEMP_DIR/mnt/nfs2/test2.bin
file-validate $TEMP_DIR/mnt/nfs99/export1/test1.bin
file-validate $TEMP_DIR/mnt/nfs99/export2/test2.bin

# Files on export97 are "metadata only", so file validation should fail
assert_failure file-validate $TEMP_DIR/mnt/nfs97/export1/test1.bin
assert_failure file-validate $TEMP_DIR/mnt/nfs97/export2/test2.bin

test_error_cleanup
