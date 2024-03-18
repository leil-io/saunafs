# Set up an installation with three disks per chunkserver
USE_RAMDISK=YES \
	CHUNKSERVERS=1 \
	DISK_PER_CHUNKSERVER=3 \
	setup_local_empty_saunafs info

# Stop daemon, damage 1st disk, start daemon again
assert_success saunafs_chunkserver_daemon 0 stop

# Clear not needed paths, every hdd line comes in the form
# [zonefs:]metadataPath[ | dataPath]
chunk0_hdd="$(sort ${info[chunkserver0_hdd]} | head -n 1)"

function containsDelimiter() {
	if [[ "${1}" = *\|* ]]; then
		echo true
	else
		echo false
	fi
}

hasDelimiter=$(containsDelimiter "${chunk0_hdd}")
isZoned=$(is_zoned_device "${chunk0_hdd}")

metaPath="$(echo $chunk0_hdd | awk '{print $1}')"
if $isZoned; then
	metaPath=$(echo $metaPath | sed s/zonefs://g)
fi
assert_success chmod 000 $metaPath

if $hasDelimiter; then
	dataPath="$(echo $chunk0_hdd | awk '{print $3}')"

	# Only apply chmod for non emulated SMR drives
	if ! $isZoned; then
		assert_success chmod 000 $dataPath
	fi
else
	dataPath="${metaPath}"
fi

assert_success saunafs_chunkserver_daemon 0 start
saunafs_wait_for_all_ready_chunkservers

# Ensure that disk 1 is damaged and other disks work
list=$(saunafs_admin_master_no_password list-disks | sort)
assert_equals 3 "$(wc -l <<< "$list")"
assert_awk_finds 'NR==1 && $4=="yes"' "$list"
assert_awk_finds 'NR==2 && $4=="no"' "$list"
assert_awk_finds 'NR==3 && $4=="no"' "$list"

# Remove the third disk from the chunkserver
sed -i -e '3s/^/#/' "${info[chunkserver0_hdd]}"
saunafs_chunkserver_daemon 0 reload
assert_eventually_prints 2 'saunafs_admin_master_no_password list-disks | wc -l'

# Expect that one line disappears from the output of saunafs-admin
# Ignore columns 6-..., because disk usage might have changed
expected_list=$(head -n2 <<< "$list" | cut -d ' ' -f 1-5)
actual_list=$(saunafs_admin_master_no_password \
	list-disks | sort | cut -d ' ' -f 1-5)
assert_no_diff "$expected_list" "$actual_list"
