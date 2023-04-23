timeout_set 3 minutes

CHUNKSERVERS=4
USE_RAMDISK=YES
MOUNT_EXTRA_CONFIG="cacheexpirationtime=0"
setup_local_empty_saunafs info

cd ${info[mount0]}

mkdir dir_std

for count in {1..2}; do
  touch dir_std/file
  FILE_SIZE=100000000 BLOCK_SIZE=2135 file-generate dir_std/file &
  file-validate-growing dir_std/file 100000000
  rm dir_std/file
done
