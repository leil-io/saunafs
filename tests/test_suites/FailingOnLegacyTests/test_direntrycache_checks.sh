#
# The goal of this test is to check the DirEntryCache behavior from the point of view of 
# the syscalls involved in its behavior. Performs checks on the currently expected behavior
# of the lookup, getattr, setattr, setxattr, mknod, mkdir, unlink, rmdir, rename, link and 
# write syscalls regarding DirEntryCache.
#
CHUNKSERVERS=1 \
	MOUNTS=2 \
	MOUNT_EXTRA_CONFIG="mfsdirentrycacheto=2" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

# A couple of functions to easily get the size and number of links to an inode
get_size() {
	ls -l $1 | cut -d " " -f 5
}

get_nliks() {
	ls -l $1 | cut -d " " -f 2
}

cd ${info[mount0]}

echo -n "1234" > file

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 4
assert_equals "$(get_nliks file)" "$(get_nliks ${info[mount1]}/file)" 1

dd if=/dev/zero of=file count=1 bs=1 seek=4 &>/dev/null

assert_equals "$(get_size file)" 5
assert_equals "$(get_size ${info[mount1]}/file)" 4

# mkdir clears cache of the parent folder
mkdir ${info[mount1]}/folder

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 5

dd if=/dev/zero of=${info[mount1]}/file count=1 bs=1 seek=5 &>/dev/null

assert_equals "$(get_size file)" 5
assert_equals "$(get_size ${info[mount1]}/file)" 6

# mknod clears cache of the parent folder
touch file2

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 6

cd folder
dd if=/dev/zero of=${info[mount1]}/file count=1 bs=1 seek=6 &>/dev/null
assert_equals "$(get_size ${info[mount0]}/file)" 6

# create only clear cache of the parent folder
echo -n "0" > file3
assert_equals "$(get_size ${info[mount0]}/file)" 6
assert_equals "$(get_size file3)" 1

dd if=/dev/zero of=${info[mount1]}/folder/file3 count=1 bs=1 seek=1 &>/dev/null

assert_equals "$(get_size ${info[mount0]}/file)" 6
assert_equals "$(get_size ${info[mount1]}/file)" 7
assert_equals "$(get_size file3)" 1
assert_equals "$(get_size ${info[mount1]}/folder/file3)" 2

# link clears cache of the involved inode and the parent folder
link "${info[mount0]}/file" link

assert_equals "$(get_nliks ${info[mount0]}/file)" 2
assert_equals "$(get_nliks ${info[mount1]}/file)" 1
assert_equals "$(get_size ${info[mount0]}/file)" "$(get_size ${info[mount1]}/file)" 7
assert_equals "$(get_size file3)" "$(get_size ${info[mount1]}/folder/file3)" 2

# setattr clears cache of the involved inode
truncate -s 6 ${info[mount1]}/folder/link
echo -n "0" > file4

# remember new file created (file4) cleared parent folder cache, but files with at least one 
# hardlink are also not kept in cache
assert_equals "$(get_size ${info[mount0]}/file)" "$(get_size ${info[mount0]}/folder/link)" 6 
assert_equals "$(get_size ${info[mount1]}/file)" "$(get_size ${info[mount1]}/folder/link)" 6
assert_equals "$(get_size file4)" "$(get_size ${info[mount1]}/folder/file4)" 1
assert_equals "$(get_nliks ${info[mount0]}/file)" "$(get_nliks ${info[mount1]}/file)" 2

dd if=/dev/zero of=${info[mount0]}/file count=1 bs=1 seek=6 &>/dev/null
dd if=/dev/zero of=file4 count=1 bs=1 seek=1 &>/dev/null

# files with at least one hardlink are not kept in cache
assert_equals "$(get_size ${info[mount1]}/file)" "$(get_size ${info[mount0]}/file)" 7
assert_equals "$(get_size file4)" 2
assert_equals "$(get_size ${info[mount1]}/folder/file4)" 1

# rename clears cache of both current and new parent folder
mv ${info[mount1]}/folder/file3 ${info[mount1]}/file3

assert_equals "$(get_size ${info[mount0]}/file)" "$(get_size ${info[mount1]}/file)" 7
assert_equals "$(get_size file4)" "$(get_size ${info[mount1]}/folder/file4)" 2

rm link
cd ..

# files with at least one hardlink are also not kept in cache
assert_equals "$(get_nliks ${info[mount1]}/file)" "$(get_nliks file)" 1

dd if=/dev/zero of=file count=1 bs=1 seek=7 &>/dev/null

assert_equals "$(get_size file)" 8
assert_equals "$(get_size ${info[mount1]}/file)" 7

# once again setattr
chmod 777 ${info[mount1]}/file

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 8

dd if=/dev/zero of=${info[mount1]}/file count=1 bs=1 seek=8 &>/dev/null

assert_equals "$(get_size file)" 8
assert_equals "$(get_size ${info[mount1]}/file)" 9

# unlink clears cache of the parent folder
unlink file2

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 9

truncate -s 7 file

assert_equals "$(get_size file)" 7
assert_equals "$(get_size ${info[mount1]}/file)" 9

# unlinking anywhere else does not clear the parent cache
rm ${info[mount1]}/folder/file4
assert_equals "$(get_size ${info[mount1]}/file)" 9

# rmdir clears cache of the parent folder
rmdir ${info[mount1]}/folder

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 7

truncate -s 5 ${info[mount1]}/file

assert_equals "$(get_size file)" 7
assert_equals "$(get_size ${info[mount1]}/file)" 5

# setxattr clears cache of the involved inode
setfacl -m u:saunafstest:r-x file

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 5

truncate -s 3 file

assert_equals "$(get_size file)" 3
assert_equals "$(get_size ${info[mount1]}/file)" 5

# readdir overrides the cache of the involved folder
ls ${info[mount1]} &>/dev/null

assert_equals "$(get_size file)" "$(get_size ${info[mount1]}/file)" 3
