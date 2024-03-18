timestamp() {
	date +%s
}

nanostamp() {
	date +%s%N
}

wait_for() {
	local goal="$1"
	local time_limit="$2"
	local end_ts="$(date +%s%N -d "$time_limit")"
	while (( $(date +%s%N) < end_ts )); do
		if eval "${goal}"; then
			return 0
		fi
		sleep 0.1
	done
	if eval "${goal}"; then
		return 0
	fi
	return 1
}

# Current Windows testing relies on masters and chunkservers running under 
# WSL1. This makes them much slower than expected, making some tests to fail
# due to failing almost-inmediate checks. Therefore, solution was to wait some
# small, though enough time (5s) to make those checks pass.
wait_if_windows() {
    if is_windows_system; then
        sleep 5
    fi
}

execution_time() {
	/usr/bin/time --quiet -f %e "${@}" 2>&1 > /dev/null || true
}
