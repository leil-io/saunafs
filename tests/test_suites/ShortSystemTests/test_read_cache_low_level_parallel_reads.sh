timeout_set 3 minutes

CHUNKSERVERS=1 \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER`
			`|cacheexpirationtime=10000`
			`|readcachemaxsizepercentage=1`
			`|sfschunkserverwavereadto=2000`
			`|sfsioretries=50`
			`|readaheadmaxwindowsize=5000`
			`|sfschunkservertotalreadto=8000" \
	setup_local_empty_saunafs info

cd ${info[mount0]}

num_jobs=18
five_percent_ram_mb=$(awk '/MemTotal/ {print int($2 / 1024 * 0.05)}' /proc/meminfo)
size_per_job=$(echo "${five_percent_ram_mb} / ${num_jobs}" | bc)
echo "five_percent_ram_mb: ${five_percent_ram_mb}, num_jobs: ${num_jobs}, size_per_job: ${size_per_job}"

fio --name=test_multiple_reads --directory=${info[mount0]} --size=${size_per_job}M --rw=write --ioengine=libaio --group_reporting --numjobs=${num_jobs} --bs=1M --direct=1 --iodepth=1
fio --name=test_multiple_reads --directory=${info[mount0]} --size=${size_per_job}M --rw=read --ioengine=libaio --group_reporting --numjobs=${num_jobs} --bs=1M --direct=1 --iodepth=1
