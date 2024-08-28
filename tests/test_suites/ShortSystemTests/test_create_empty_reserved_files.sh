CHUNKSERVERS=1 \
	MOUNTS=2
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="EMPTY_RESERVED_FILES_PERIOD_MSECONDS = 1000"\
	MOUNT_0_EXTRA_CONFIG="sfscachemode=NEVER" \
	MOUNT_1_EXTRA_CONFIG="sfsmeta" \
	SFSEXPORTS_META_EXTRA_OPTIONS="nonrootmeta" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

# Function to count the number of files in a given directory
count_files() {
    local dir_path="$1"
    echo $(ls "$dir_path" | wc -l)
}

# set trash and reserved files folder
trash="${info[mount1]}/trash"
reserved="${info[mount1]}/reserved"

mkdir folder

# set folder trash time to 0 for redirecting 
# files in use to reserved files folder
saunafs settrashtime 0 folder

# create 5 files and keep them in use
for i in {1..5}; do
    touch folder/file$i
    while true; do echo "Data" >> folder/file$i; sleep 1; done &
done

# delete those 5 files and as trashtime = 0
# then these files are marked as reserved in the system
for i in {1..5}; do
    rm folder/file$i
done

# check files are not in trash folder, 
# just undel folder
trash_files_count=$(count_files $trash)
echo "number of trash files: $trash_files_count"
assert_equals "1" "$trash_files_count"

# check there are 5 files on reserved files
reserved_files_count=$(count_files $reserved)
echo "number of reserved files: $reserved_files_count"
assert_equals "5" "$reserved_files_count"

# sleep for a enough period to allow cleanup
# of reserved files
sleep 5

# check reserved files were correctly deleted
reserved_files_count_after=$(count_files $reserved)
echo "number of reserved files after cleanup: $reserved_files_count_after"
assert_equals "0" "$reserved_files_count_after"
