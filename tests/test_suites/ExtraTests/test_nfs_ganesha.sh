#
# To run this test you need to add the following lines to /etc/sudoers.d/lizardfstest:
#
# lizardfstest ALL = NOPASSWD: /bin/mount, /bin/umount, /bin/pkill, /bin/mkdir, /bin/touch
# lizardfstest ALL = NOPASSWD: /tmp/LizardFS-autotests/mnt/mfs0/bin/ganesha.nfsd
#
# The path for the Ganesha daemon should match the installation folder inside the test.
#

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
mkdir -p ${TEMP_DIR}/mnt/ganesha/cthon_test
mkdir -p ${info[mount0]}/data

MAX_PATH_DEPH=9

USER_ID=$(id -u lizardfstest)
GROUP_ID=$(id -g lizardfstest)
GANESHA_BS="$((1<<20))"

# Create some directories at data
cd ${info[mount0]}/data
for i in $(seq 1 ${MAX_PATH_DEPH}); do
	mkdir -p dir${i}
	cd dir${i}
done

# Create a file at data
touch ./file
echo 'Ganesha_Test_Ok' > ./file
INODE=$(stat -c %i ./file)

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
CC="ccache gcc" cmake -DCMAKE_INSTALL_PREFIX=${info[mount0]} -DUSE_GSS=NO ..
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
	Export_Id = 99;
	Path = /data;
	Pseudo = /data;
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
sudo mount -vvvv localhost:/data $TEMP_DIR/mnt/ganesha

cat <<EOF > "${TEMP_DIR}/mnt/ganesha/cthon04.patch"
diff --git a/lock/tlock.c b/lock/tlock.c
index 8c837a8..7060cca 100644
--- a/lock/tlock.c
+++ b/lock/tlock.c
@@ -479,21 +479,21 @@ fmtrange(offset, length)
 #ifdef LARGE_LOCKS                     /* non-native 64-bit */
        if (length != 0)
-               sprintf(buf, "[%16llx,%16llx] ", offset, length);
+               sprintf(buf, "[%16lx,%16lx] ", offset, length);
        else
-               sprintf(buf, "[%16llx,          ENDING] ", offset);
+               sprintf(buf, "[%16lx,          ENDING] ", offset);
 #else /* LARGE_LOCKS */
        if (sizeof (offset) == 4) {
                if (length != 0)
-                       sprintf(buf, "[%8lx,%8lx] ", (int32_t)offset,
+                       sprintf(buf, "[%i,%i] ", (int32_t)offset,
                                (int32_t)length);
                else
-                       sprintf(buf, "[%8lx,  ENDING] ", (int32_t)offset);
+                       sprintf(buf, "[%i,  ENDING] ", (int32_t)offset);
        } else {
                if (length != 0)
-                       sprintf(buf, "[%16llx,%16llx] ", offset, length);
+                       sprintf(buf, "[%16lx,%16lx] ", offset, length);
                else
-                       sprintf(buf, "[%16llx,          ENDING] ", offset);
+                       sprintf(buf, "[%16lx,          ENDING] ", offset);
        }
 #endif /* LARGE_LOCKS */
EOF

sudo -i -u lizardfstest bash << EOF
 cd "${TEMP_DIR}/mnt/ganesha"
 chmod o+w "$(pwd)"
 if test -d "${TEMP_DIR}/mnt/ganesha/cthon04"; then
	rm -rf "${TEMP_DIR:?}/mnt/ganesha/cthon04"
 fi
 git clone --no-checkout git://git.linux-nfs.org/projects/steved/cthon04.git
 cd cthon04
 git reset --hard HEAD
 git apply --ignore-whitespace "${TEMP_DIR}/mnt/ganesha/cthon04.patch"
 make all
 export NFSTESTDIR=${TEMP_DIR}/mnt/ganesha/cthon_test
 ./runtests -b -n
 ./runtests -l -n
 ./runtests -s -n
EOF

cd ${TEMP_DIR}/mnt/ganesha
MAX_FILES=300
### Check mkdir / rmdir syscall
mkdir -p ./dir_on_ganesha
test -d ./dir_on_ganesha
rmdir ./dir_on_ganesha
test ! -d ./dir_on_ganesha

for i in $(seq ${MAX_FILES}); do
	touch ./file.${i};
	test -f ./file.${i};
done

### Check getattr / stat / syscall
STATS_REPORT="$(stat "$(find -name file)")"
assert_equals ${INODE} "$(echo "${STATS_REPORT}" | grep -i inode | cut -d: -f3 | cut -d" " -f2)"
assert_equals ${USER_ID} "$(echo "${STATS_REPORT}" | grep -i uid | cut -d/ -f2 | rev | awk '{print $1}' | rev)"
assert_equals ${GROUP_ID} "$(echo "${STATS_REPORT}" | grep -i gid | cut -d/ -f3 | rev | awk '{print $1}' | rev)"
assert_equals ${GANESHA_BS} "$(echo "${STATS_REPORT}" | grep -i 'io block' | cut -d: -f4 | awk '{print $1}')"

### Check readdir / ls|tree / syscall
assert_equals $((${MAX_PATH_DEPH} + 12)) $(tree ${TEMP_DIR}/mnt/ganesha | grep directories | awk '{print $1}')
assert_equals "$((${MAX_FILES} + 3))" "$(ls ${TEMP_DIR}/mnt/ganesha/ | wc -l)"

### Check open2 / cat|dd / syscall
assert_equals "Ganesha_Test_Ok" $(cat $(find -name file))

test_error_cleanup || true
