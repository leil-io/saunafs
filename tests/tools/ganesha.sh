#!/usr/bin/env bash

# Function to retry the specified command up to a maximum number of attempts
# with a delay of 5 seconds between each attempt
retry_command_with_attempts() {
	local max_attempts=$1
	local attempt=1
	local delay=5
	shift
	while (( attempt <= max_attempts )); do
		if "$@"; then
			return 0
		fi
		echo "Attempt $attempt failed! Retrying in $delay seconds..."
		sleep $delay
		attempt=$(( attempt + 1 ))
	done

	echo "All attempts failed!"
	return 1
}

# Create PID file for Ganesha
create_ganesha_pid_file() {
	PID_FILE=/var/run/ganesha/ganesha.pid
	if [ ! -f ${PID_FILE} ]; then
		echo "ganesha.pid doesn't exists, creating it..."
		sudo mkdir -p /var/run/ganesha
		sudo touch "${PID_FILE}"
	fi
}

# Function to check RPC service availability
check_and_recover_rpc_service() {
	# if NFS service is available, RPC is already running
	if [ showmount -e localhost ]; then
		echo "RPC service is now available"
		return 0
	fi

	rpcbind_pid=$(pidof rpcbind)

	# Check if rpcbind is running and restart it
	if [ -n "$rpcbind_pid" ]; then
		echo "RPC service is unavailable"
		sudo pkill -HUP rpcbind
	fi

	return 1
}

check_rpc_service() {
	if ! retry_command_with_attempts 5 showmount -e localhost; then
		# If NFS service is not available, try to recover RPC service
		retry_command_with_attempts 5 check_and_recover_rpc_service
	fi
}
