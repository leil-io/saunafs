timeout_set 2 minutes

CHUNKSERVERS=12 \
    USE_RAMDISK=YES \
    MOUNT_EXTRA_CONFIG="readaheadmaxwindowsize=256,maxreadaheadrequests=30,readworkers=100" \
    MASTER_CUSTOM_GOALS="8 ec_8_4: \$ec(8,4)"
    setup_local_empty_saunafs info

cd ${info[mount0]}

output_dir="random_files"
mkdir -p "${output_dir}"
touch control.files.saunafs

saunafs setgoal -r ec_8_4 "${output_dir}"
saunafs setgoal ec_8_4 control.files.saunafs

# Generate 50 files
for i in {1..50}; do
    size=$(shuf -i 5000000-35000000 -n 1)
    FILE_SIZE=${size} BLOCK_SIZE=65536 file-generate "${output_dir}/file_${i}"
    echo $(pwd)/${output_dir}/file_${i} >> control.files.saunafs
done

echo "Dropping cache"
drop_caches

echo "Calculating parallel checksums"
cat control.files.saunafs | while read -e line;
do
    md5sum "${line}" > $(mktemp /tmp/tmp.XXXXXXXXX.md5sum) &
done

# Progress bar
while pgrep md5sum > /dev/null; do
    echo -n "$(pgrep md5sum | wc -l)..."
    sleep 1
done

sync

# Check how many threads created output
echo ""
ls /tmp/tmp.*.md5sum | wc -l

# Generate one md5sum file
cat /tmp/tmp.*.md5sum > /tmp/parallel.md5sum.result.v2

# Clean up
rm /tmp/tmp.*.md5sum

echo "Dropping cache"
drop_caches

echo "Checking ... by reading single threaded md5sums"
assert_equals $(md5sum --check /tmp/parallel.md5sum.result.v2 | egrep "FAILED" | wc -l) 0

echo "Validating files"
for i in {1..50}; do
    assert_success file-validate "${output_dir}/file_${i}" &
done
