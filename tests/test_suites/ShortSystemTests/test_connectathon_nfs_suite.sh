timeout_set 70 seconds

CHUNKSERVERS=3 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="mfscachemode=NEVER,enablefilelocks=1,cacheexpirationtime=0" \
	CHUNKSERVER_EXTRA_CONFIG="READ_AHEAD_KB = 1024|MAX_READ_BEHIND_KB = 2048"
	setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir cthon_tests
export NFSTESTDIR=${info[mount0]}/cthon_tests

git clone https://github.com/leil-io/cthon04.git
cd cthon04
make all

./runtests -b -n
./runtests -l -n
./runtests -s -n

