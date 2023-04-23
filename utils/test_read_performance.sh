#!/usr/bin/env bash
set -e
print_usage() {
	cat >&2 <<EOF
Usage: $0 FILE

This script runs performance tests on some scenarios using the fio tool.
* If the file provided does not exist, fio will create a 16GB file in its place.
* If the file provided does not reach the 16GB in size, fio will fill until the file reaches 16GB.

Example:
$0 /mnt/saunafs/big_file
$0 big_file
EOF
	exit 1
}

ensure_fio_installed() {
	if ! which fio >/dev/null; then
		echo "fio not installed"
		sudo apt -y install fio || exit 1
	fi
}

get_value_from_arg () {
	local key="$1"
	echo "${args}" | grep -oP '(?<=-'"${key}"'=)[^ ]+' | head -1
}
generate_description() {
	local args="$@"
	local descriptions=()
	while [[ "${args}" =~ "-rw=" ]]; do
		local rw_type=$(get_value_from_arg rw)
		args=${args#*-rw=}

		local num_jobs=$(get_value_from_arg numjobs)
		args=${args#*-numjobs=}

		local delay=$(get_value_from_arg thinktime)
		args=${args#*-thinktime=}

		case "${rw_type}" in
			read)     rw_desc="Sequential Read" ;;
			randread) rw_desc="Random Read" ;;
			*)        rw_desc="${rw_type}" ;;
		esac

		case "${delay}" in
			0ms)     delay_desc="FAST" ;;
			*)       delay_desc="with ${delay} delay" ;;
		esac


		[[ "${num_jobs}" == "1" ]] && echo      "Single ${rw_desc} ${delay_desc}"
		[[ "${num_jobs}"  > "1" ]] && echo "${num_jobs} ${rw_desc} ${delay_desc}"

	done
}

run_fio_test() {
	echo 3 > /proc/sys/vm/drop_caches
	local thinktime=$(echo "$@" | grep -oP '(?<=-thinktime=)[^ ]+')
	[[ -z "${thinktime}" ]] && set -- "$@" "-thinktime=0ms"

	local description=$(generate_description "$@")
	echo ""
	sed 's/./-/g' <<< ${description} | sort -r | head -1
	echo "${description}"
	sed 's/./-/g' <<< ${description} | sort -r | head -1
	fio -filename="${file}" "$@" |& tee -a "${log_file}" | grep --color -E 'READ'
}

file="$1"
log_file="/tmp/$(basename ""$0"")_$(date '+%F_%H%M%S').log"

if [[ -z "${file}" ]]; then
	print_usage
fi

ensure_fio_installed

# Test invocations
run_fio_test -direct=1 -rw=read     -numjobs=1   -bs=1MB   -size=16GB -runtime=300 -thinktime=0ms   -name=seqread

run_fio_test -direct=1 -rw=randread -numjobs=1   -bs=1MB   -size=16GB -runtime=300 -thinktime=0ms   -name=randread

run_fio_test -direct=1 -rw=read     -numjobs=5   -bs=1MB   -size=16GB -runtime=300 -thinktime=0ms   -name=seqreads_5

run_fio_test -direct=1 -rw=randread -numjobs=5   -bs=1MB   -size=16GB -runtime=300 -thinktime=0ms   -name=randreads_5

run_fio_test -direct=1 -rw=read     -numjobs=128 -bs=256KB -size=32MB -runtime=500 -thinktime=125ms -name=slowseqreads_128 \
                       -rw=read     -numjobs=1   -bs=1MB   -size=16GB -runtime=300 -thinktime=0ms   -name=fastseqread

run_fio_test -direct=1 -rw=randread -numjobs=128 -bs=256KB -size=32MB -runtime=500 -thinktime=125ms -name=slowrandreads_128 \
                       -rw=read     -numjobs=1   -bs=1MB   -size=16GB -runtime=300 -thinktime=0ms   -name=fastseqread

echo
echo Full log is available here:
echo " cat ${log_file}"
