timeout_set 2 minutes

MOUNT_EXTRA_CONFIG="sfscachemode=NEVER`
		`|readcachemaxsizepercentage=1`
		`|maxreadaheadrequests=0`
		`|sfsioretries=14`
		`|cacheexpirationtime=10000" \
    setup_local_empty_saunafs info

cd "${info[mount0]}"

# Calculate total system memory in MB
total_memory_mb=$(free --mega | awk '/^Mem:/ {print $2}')

# Calculate 2% of total memory
two_percent_memory=$(echo "${total_memory_mb} * 0.02" | bc | awk '{printf "%.0f", $0}')

# Since 'dd' deals with whole numbers, round the required 
# size in MB for 2% of memory to the nearest whole number.
required_size_mb=$(printf "%.0f" "${two_percent_memory}")

# Generate a file that is 2% of total memory size
FILE_SIZE=${required_size_mb}M BLOCK_SIZE=1M file-generate test_data

# Create a copy of the file to validate generated data
dd if=test_data of=test_data_copy bs=1M

# Perform file validation on copied test data
file-validate test_data_copy
