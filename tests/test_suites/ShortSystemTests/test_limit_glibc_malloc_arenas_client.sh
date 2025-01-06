timeout_set 5 minutes

CHUNKSERVERS=1 \
	MOUNTS=2 \
    MOUNT_EXTRA_CONFIG="sfscachemode=NEVER`
			`|cacheexpirationtime=10000`
			`|readcachemaxsizepercentage=1`
			`|sfschunkserverwavereadto=2000`
			`|sfsioretries=50`
			`|readaheadmaxwindowsize=5000`
			`|sfschunkservertotalreadto=8000" \
	MOUNT_0_EXTRA_CONFIG="limitglibcmallocarenas=8" \
	MOUNT_1_EXTRA_CONFIG="limitglibcmallocarenas=2" \
    setup_local_empty_saunafs info

num_jobs=18
five_percent_ram_mb=$(awk '/MemTotal/ {print int($2 / 1024 * 0.05)}' /proc/meminfo)
size_per_job=$(echo "${five_percent_ram_mb} / ${num_jobs}" | bc)
echo "five_percent_ram_mb: ${five_percent_ram_mb}, num_jobs: ${num_jobs}, size_per_job: ${size_per_job}"

pgrep -fa sfsmount
# Get the PIDs of the sfsmount processes
pids=($(pgrep -fa sfsmount | awk '{print $1}'))

# Access the PIDs separately
pid1=${pids[0]}
pid2=${pids[1]}

echo "pids: ${pids[@]}"
echo "pid1: $pid1, pid2: $pid2"

function getVirtualMemoryForPid() {
	pid=${1}
	ps -o vsz= -p ${pid}
}

# Function to run fio commands and measure VSZ for a given mount point
run_fio_and_measure_vsz() {
	# Prepare measure the average virtual memory usage during the fio read command
	local mount_point=$1
	local pid=$2
	local output_file="pidstat_output_$pid.txt"

	cd "$mount_point"
	
	fio --name=test_multiple_reads --directory=$mount_point --size=${size_per_job}M --rw=write --ioengine=libaio --group_reporting --numjobs=${num_jobs} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1
	
	# Run the fio read command in the background
	fio --name=test_multiple_reads --directory=$mount_point --size=${size_per_job}M --rw=read --ioengine=libaio --group_reporting --numjobs=${num_jobs} --bs=1M --direct=1 --iodepth=1 > /dev/null 2>&1 &
	
	# Store the fio process ID
	fio_pid=$(pgrep -f "fio --name=test_multiple_reads --directory=$mount_point --size=${size_per_job}M --rw=read")

	# Run pidstat in the background to monitor virtual memory usage
	sleep 1
	virtualMemory=$(getVirtualMemoryForPid $pid)

	# Wait for the fio command to complete
	wait $fio_pid

	# Calculate the average virtual memory usage
	echo $virtualMemory
}

# Run the fio commands and measure VSZ for both mount points
echo "Running fio commands and measuring VSZ for both mount points"
avg_vsz_mount0=$(run_fio_and_measure_vsz "${info[mount0]}" $pid1)
avg_vsz_mount1=$(run_fio_and_measure_vsz "${info[mount1]}" $pid2)

# Print the average VSZ values
echo "Average VSZ for ${info[mount0]}: ${avg_vsz_mount0}"
echo "Average VSZ for ${info[mount1]}: ${avg_vsz_mount1}"

assert_less_than "${avg_vsz_mount1}" "${avg_vsz_mount0}"
