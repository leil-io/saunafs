CHUNKSERVERS=5 \
	MOUNTS=2 \
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER`
			`|sfsignoreflush=1`
			`|sfsioretries=5" \
	MOUNT_1_EXTRA_CONFIG="sfscachemode=NEVER`
			`|sfsignoreflush=0`
			`|sfsioretries=5" \
	MASTER_CUSTOM_GOALS="10 ec_3_2: \$ec(3,2)" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

get_checksum() {
	sha256sum $1 | awk '{ print $1 }'
}

dir="${info[mount0]}/dir1"
dir2="${info[mount1]}/dir2"

mkdir "$dir" "$dir2"

saunafs setgoal ec_3_2 "$dir" "$dir2"

# stop three chunkservers for invalidating the writing of a ec_3_2 file
for i in {0..2}; do
	saunafs_chunkserver_daemon $i stop
done

assert_success dd if=/dev/zero of=${dir}/file1 bs=1M count=1 oflag=direct
assert_failure dd if=/dev/zero of=${dir2}/file2 bs=1M count=1 oflag=direct

# now test that with only two chunkservers stopped, 
# writing the ec_3_2 files is possible
saunafs_chunkserver_daemon 0 start

dd if=/dev/urandom of=/tmp/tmpfile1 bs=1M count=100
dd if=/dev/urandom of=/tmp/tmpfile2 bs=1M count=100

checksum_tmp_file1=$(get_checksum /tmp/tmpfile1)
checksum_tmp_file2=$(get_checksum /tmp/tmpfile2)

echo "Copying files from /tmp to $dir and $dir2"
assert_success dd if=/tmp/tmpfile1 of=${dir}/tmpfile1 oflag=direct bs=1M count=100
assert_success dd if=/tmp/tmpfile2 of=${dir2}/tmpfile2 oflag=direct bs=1M count=100

echo "Compare files to ensure they are identical"
assert_success cmp /tmp/tmpfile1 ${dir}/tmpfile1
assert_success cmp /tmp/tmpfile2 ${dir2}/tmpfile2

echo "Get the md5sum of the files"
checksum_dir_file1=$(get_checksum ${dir}/tmpfile1)
checksum_dir_file2=$(get_checksum ${dir2}/tmpfile2)

echo "Compare md5sums to ensure they are identical"
assert_equals $checksum_tmp_file1 $checksum_dir_file1
assert_equals $checksum_tmp_file2 $checksum_dir_file2
