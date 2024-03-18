# Usage: setup_local_empty_saunafs out_var
# Configures and starts master, chunkserver and mounts
# If out_var provided an associative array with name $out_var
# is created and it contains information about the filestystem
setup_local_empty_saunafs() {
	local use_legacy=${USE_LEGACY:-}
	local use_saunafsXX=${START_WITH_LEGACY_SAUNAFS:-}
	local use_ramdisk=${USE_RAMDISK:-}
	local use_zoned_disks=${USE_ZONED_DISKS:-}
	local zone_size_mb=${ZONE_SIZE_MB:-256}
	local number_of_conv_zones=${NUMBER_OF_CONV_ZONES:-9}
	local number_of_seq_zones=${NUMBER_OF_SEQ_ZONES:-16}
	local use_loop=${USE_LOOP_DISKS:-}
	local number_of_masterservers=${MASTERSERVERS:-1}
	local number_of_chunkservers=${CHUNKSERVERS:-1}
	local number_of_mounts=${MOUNTS:-1}
	local disks_per_chunkserver=${DISK_PER_CHUNKSERVER:-1}
	local auto_shadow_master=${AUTO_SHADOW_MASTER:-YES}
	local cgi_server=${CGI_SERVER:-NO}
	local ip_address=$(get_ip_addr)
	local etcdir=$TEMP_DIR/saunafs/etc
	local vardir=$TEMP_DIR/saunafs/var
	local mntdir=$TEMP_DIR/mnt
	local master_start_param=${MASTER_START_PARAM:-}
	local shadow_start_param=${SHADOW_START_PARAM:-}
	declare -gA saunafs_info_
	saunafs_info_[chunkserver_count]=$number_of_chunkservers
	saunafs_info_[admin_password]=${ADMIN_PASSWORD:-password}

	# Enable always ramdisk for zoned devices
	if parse_true "${use_zoned_disks}"; then
		use_ramdisk="true"
	fi

	declare -g ZONED_DISKS="${use_zoned_disks}"
	export ZONED_DISKS

	# Try to enable core dumps if possible
	if [[ $(ulimit -c) == 0 ]]; then
		ulimit -c unlimited || ulimit -c 100000000 || ulimit -c 1000000 || ulimit -c 10000 || :
	fi

	# Prepare directories for SaunaFS
	mkdir -p "$etcdir" "$vardir"

	use_new_goal_config="true"
	local oldpath="$PATH"
	if [[ $use_legacy ]]; then
		use_new_goal_config="false"
		export PATH="$LEGACY_DIR/bin:$LEGACY_DIR/sbin:$PATH"
		build_legacy
	fi

	if [[ $use_saunafsXX ]]; then
		# In old suite, when legacy version was >= 3.11, we set `use_new_goal_config`=true
		# Maybe that's not what we want? (look at if above)
		use_new_goal_config="true"
		SAUNAFSXX_DIR=${SAUNAFSXX_DIR_BASE}/install/usr
		export PATH="${SAUNAFSXX_DIR}/bin:${SAUNAFSXX_DIR}/sbin:$PATH"
		install_saunafsXX
	fi

	# Prepare configuration for metadata servers
	use_new_format=$use_new_goal_config prepare_common_metadata_server_files_
	add_metadata_server_ 0 "master"
	for ((msid = 1; msid < number_of_masterservers; ++msid)); do
		add_metadata_server_ $msid "shadow"
	done
	saunafs_info_[current_master]=0
	saunafs_info_[master_cfg]=${saunafs_info_[master0_cfg]}
	saunafs_info_[master_data_path]=${saunafs_info_[master0_data_path]}
	saunafs_info_[masterserver_count]=$number_of_masterservers

	# Start one masterserver with personality master
	saunafs_master_daemon start ${master_start_param}

	# Prepare the metalogger, so that any test can start it
	prepare_metalogger_

	# Start chunkservers, but first check if he have enough disks
	if [[ ! $use_ramdisk ]]; then
		if [[ $use_loop ]]; then
			local disks=($SAUNAFS_LOOP_DISKS)
		else
			local disks=($SAUNAFS_DISKS)
		fi
		local disks_needed=$((number_of_chunkservers * disks_per_chunkserver))
		local disks_available=${#disks[@]}
		if ((disks_available < disks_needed)); then
			test_fail "Test needs $disks_needed disks" \
				"but only $disks_available (${disks[@]-}) are available"
		fi
	fi

	for ((csid = 0; csid < number_of_chunkservers; ++csid)); do
		add_chunkserver_ $csid
	done

	# Mount the filesystem
	for ((mntid = 0; mntid < number_of_mounts; ++mntid)); do
		add_mount_ $mntid
	done

	export PATH="$oldpath"

	# Add shadow master if not present (and not disabled); wait for it to synchronize
	if [[ $auto_shadow_master == YES && $number_of_masterservers == 1 ]]; then
		add_metadata_server_ auto "shadow"
		saunafs_master_n auto start ${shadow_start_param}
		assert_eventually 'saunafs_shadow_synchronized auto'
	fi

	if [[ $cgi_server == YES ]]; then
		add_cgi_server_
	fi

	# Wait for chunkservers (use saunafs-admin only for SaunaFS -- MooseFS doesn't support it)
	if [[ ! $use_legacy ]]; then
		saunafs_wait_for_all_ready_chunkservers
	else
		sleep 3 # A reasonable fallback
	fi

	# Return array containing information about the installation
	local out_var=$1
	unset "$out_var"
	declare -gA "$out_var" # Create global associative array, requires bash 4.2
	for key in "${!saunafs_info_[@]}"; do
		eval "$out_var['$key']='${saunafs_info_[$key]}'"
	done
}

saunafs_fusermount() {
	fuse_version=$(${SAFS_MOUNT_COMMAND} --version 2>&1 | grep "FUSE library" | grep -Eo "[0-9]+\..+")
	if [[ "$fuse_version" =~ ^3\..+$ ]]; then
		fusermount3 "$@"
	else
		fusermount "$@"
	fi
}

# saunafs_chunkserver_daemon <id> start|stop|restart|kill|tests|isalive|...
saunafs_chunkserver_daemon() {
	local id=$1
	shift
	sfschunkserver -c "${saunafs_info_[chunkserver${id}_cfg]}" "$@" | cat
	return ${PIPESTATUS[0]}
}

saunafs_master_daemon() {
	sfsmaster -c "${saunafs_info_[master${saunafs_info_[current_master]}_cfg]}" "$@" | cat
	return ${PIPESTATUS[0]}
}

# saunafs_master_daemon start|stop|restart|kill|tests|isalive|...
saunafs_master_n() {
	local id=$1
	shift
	sfsmaster -c "${saunafs_info_[master${id}_cfg]}" "$@" | cat
	return ${PIPESTATUS[0]}
}

# saunafs_metalogger_daemon start|stop|restart|kill|tests|isalive|...
saunafs_metalogger_daemon() {
	sfsmetalogger -c "${saunafs_info_[metalogger_cfg]}" "$@" | cat
	return ${PIPESTATUS[0]}
}

# saunafs_mount_unmount_async <id>
saunafs_mount_unmount_async() {
	local mount_id=$1
	local mount_dir=${saunafs_info_[mount${mount_id}]}
	saunafs_fusermount -u ${mount_dir}
}

# saunafs_mount_unmount <id> <timeout>
saunafs_mount_unmount() {
	local mount_id=$1
	local timeout=${2:-'5 seconds'}
	local mount_cmd="${saunafs_info_[mnt${mount_id}_command]}"
	local mount_dir="${saunafs_info_[mount${mount_id}]}"
	local test_cmd="! pgrep -f -u saunafstest '\\<${mount_cmd}\\>.*${mount_dir}' >/dev/null"
	sync "${mount_dir}"
	saunafs_mount_unmount_async ${mount_id}
	wait_for "${test_cmd}" "$timeout"
	local test_mount="grep -q '${mount_dir}' /proc/mounts"
	assert_failure "${test_mount}"
}

# saunafs_mount_start <id> <mount_cmd>
saunafs_mount_start() {
	local mount_id=$1
	local mount_cmd=${2:-}
	if [[ $mount_cmd ]]; then
		configure_mount_ ${mount_id} ${mount_cmd}
	fi
	do_mount_ ${mount_id}
}

# A bunch of private function of this module

create_sfsexports_cfg_() {
	local base="* / rw,alldirs,maproot=0"
	local meta_base="* . rw"
	local additional=${SFSEXPORTS_EXTRA_OPTIONS-}
	local meta_additional=${SFSEXPORTS_META_EXTRA_OPTIONS-}
	if [[ $additional ]]; then
		additional=",$additional"
	fi
	if [[ $meta_additional ]]; then
		meta_additional=",$meta_additional"
	fi
	# general entries
	echo "${base}${additional}"
	echo "${meta_base}${meta_additional}"
	# per-mount entries
	for ((mntid = 0; mntid < number_of_mounts; ++mntid)); do
		local this_mount_exports_variable="MOUNT_${mntid}_EXTRA_EXPORTS"
		local this_mount_exports=${!this_mount_exports_variable-}
		if [[ ${this_mount_exports} ]]; then
			echo "${base},password=${mntid}${additional},${this_mount_exports}"
		fi
	done
}

create_sfsgoals_cfg_() {
	local goal_name

	for i in {1..5}; do
		echo "${MASTER_CUSTOM_GOALS:-}" | tr '|' '\n' | grep "^$i " | cat
	done
	for i in {6..20}; do
		wildcards=
		for ((j = 0; j < i; j++)); do
			wildcards="$wildcards _"
		done
		(echo "${MASTER_CUSTOM_GOALS:-}" | tr '|' '\n' | grep "^$i ") || echo "$i $i: $wildcards"
	done
	if [[ $use_new_format == "true" ]]; then
		for i in {2..9}; do
			echo "$((21 + $i)) xor$i: \$xor$i"
		done
		for i in {2..4}; do
			echo "$((31 + 2 * (i - 2) + 0)) ec$i$((i - 1)): \$ec($i,$((i - 1)))"
			echo "$((31 + 2 * (i - 2) + 1)) ec$i$i: \$ec($i,$i)"
		done
	fi
}

create_sfstopology_cfg_() {
	echo '# empty topology...'
}

# Creates MAGIC_DEBUG_LOG which will cause test to fail is some error is logged by any daemon
create_magic_debug_log_entry_() {
	local servername=$1

	# By default, fail on all prefixes passed in DEBUG_LOG_FAIL_ON
	local prefixes=${DEBUG_LOG_FAIL_ON:-}

	local prefix
	# Create MAGIC_DEBUG_LOG_C config entry from all requested prefixes
	if [[ $prefixes ]]; then
		echo -n "MAGIC_DEBUG_LOG_C = "
		for prefix in $prefixes; do
			echo -n "$prefix:$ERROR_DIR/debug_log_errors_${servername}.log,"
		done
		echo
	fi | sed -e 's/,$//'
}

# Sometimes use Berkley DB name storage
create_bdb_name_storage_entry_() {
	if (($RANDOM % 2)); then
		echo "USE_BDB_FOR_NAME_STORAGE = 0"
	else
		echo "USE_BDB_FOR_NAME_STORAGE = 1"
	fi
}

create_sfsmaster_master_cfg_() {
	local this_module_cfg_variable="MASTER_${masterserver_id}_EXTRA_CONFIG"
	echo "PERSONALITY = master"
	echo "SYSLOG_IDENT = master_${masterserver_id}"
	echo "WORKING_USER = $(id -nu)"
	echo "WORKING_GROUP = $(id -ng)"
	echo "EXPORTS_FILENAME = ${saunafs_info_[master_exports]}"
	echo "TOPOLOGY_FILENAME = ${saunafs_info_[master_topology]}"
	echo "CUSTOM_GOALS_FILENAME = ${saunafs_info_[master_custom_goals]}"
	echo "DATA_PATH = $masterserver_data_path"
	echo "MATOML_LISTEN_PORT = ${saunafs_info_[matoml]}"
	echo "MATOCS_LISTEN_PORT = ${saunafs_info_[matocs]}"
	echo "MATOCL_LISTEN_PORT = ${saunafs_info_[matocl]}"
	echo "MATOTS_LISTEN_PORT = ${saunafs_info_[matots]}"
	echo "METADATA_CHECKSUM_INTERVAL = 1"
	echo "ADMIN_PASSWORD = ${saunafs_info_[admin_password]}"
	create_magic_debug_log_entry_ "master_${masterserver_id}"
	echo "${MASTER_EXTRA_CONFIG-}" | tr '|' '\n'
	echo "${!this_module_cfg_variable-}" | tr '|' '\n'
	create_bdb_name_storage_entry_
}

create_sfsmaster_shadow_cfg_() {
	local this_module_cfg_variable="MASTER_${masterserver_id}_EXTRA_CONFIG"
	echo "PERSONALITY = shadow"
	echo "SYSLOG_IDENT = shadow_${masterserver_id}"
	echo "WORKING_USER = $(id -nu)"
	echo "WORKING_GROUP = $(id -ng)"
	echo "EXPORTS_FILENAME = ${saunafs_info_[master_exports]}"
	echo "TOPOLOGY_FILENAME = ${saunafs_info_[master_topology]}"
	echo "CUSTOM_GOALS_FILENAME = ${saunafs_info_[master_custom_goals]}"
	echo "DATA_PATH = $masterserver_data_path"
	echo "MATOML_LISTEN_PORT = $masterserver_matoml_port"
	echo "MATOCS_LISTEN_PORT = $masterserver_matocs_port"
	echo "MATOCL_LISTEN_PORT = $masterserver_matocl_port"
	echo "MATOTS_LISTEN_PORT = $masterserver_matots_port"
	echo "MASTER_HOST = $(get_ip_addr)"
	echo "MASTER_PORT = ${saunafs_info_[matoml]}"
	echo "METADATA_CHECKSUM_INTERVAL = 1"
	echo "ADMIN_PASSWORD = ${saunafs_info_[admin_password]}"
	create_magic_debug_log_entry_ "shadow_${masterserver_id}"
	echo "${MASTER_EXTRA_CONFIG-}" | tr '|' '\n'
	echo "${!this_module_cfg_variable-}" | tr '|' '\n'
	create_bdb_name_storage_entry_
}

saunafs_make_conf_for_shadow() {
	local target=$1
	cp -f "${saunafs_info_[master${target}_shadow_cfg]}" "${saunafs_info_[master${target}_cfg]}"
}

saunafs_make_conf_for_master() {
	local new_master=$1
	local old_master=${saunafs_info_[current_master]}
	# move master responsibility to new masterserver
	cp -f "${saunafs_info_[master${new_master}_master_cfg]}" "${saunafs_info_[master${new_master}_cfg]}"
	saunafs_info_[master_cfg]=${saunafs_info_[master${new_master}_master_cfg]}
	saunafs_info_[master_data_path]=${saunafs_info_[master${new_master}_data_path]}
	saunafs_info_[current_master]=$new_master
}

saunafs_current_master_id() {
	echo ${saunafs_info_[current_master]}
}

prepare_common_metadata_server_files_() {
	create_sfsexports_cfg_ >"$etcdir/sfsexports.cfg"
	create_sfstopology_cfg_ >"$etcdir/sfstopology.cfg"
	create_sfsgoals_cfg_ >"$etcdir/sfsgoals.cfg"
	saunafs_info_[master_exports]="$etcdir/sfsexports.cfg"
	saunafs_info_[master_topology]="$etcdir/sfstopology.cfg"
	saunafs_info_[master_custom_goals]="$etcdir/sfsgoals.cfg"
	get_next_port_number "saunafs_info_[matoml]"
	get_next_port_number "saunafs_info_[matocl]"
	get_next_port_number "saunafs_info_[matocs]"
	get_next_port_number "saunafs_info_[matots]"
}

add_metadata_server_() {
	local masterserver_id=$1
	local personality=$2

	local masterserver_matoml_port
	local masterserver_matocl_port
	local masterserver_matocs_port
	local masterserver_matots_port
	local masterserver_data_path=$vardir/master${masterserver_id}
	local masterserver_master_cfg=$etcdir/sfsmaster${masterserver_id}_master.cfg
	local masterserver_shadow_cfg=$etcdir/sfsmaster${masterserver_id}_shadow.cfg
	local masterserver_cfg=$etcdir/sfsmaster${masterserver_id}.cfg

	get_next_port_number masterserver_matoml_port
	get_next_port_number masterserver_matocl_port
	get_next_port_number masterserver_matocs_port
	get_next_port_number masterserver_matots_port
	mkdir "$masterserver_data_path"
	create_sfsmaster_master_cfg_ >"$masterserver_master_cfg"
	create_sfsmaster_shadow_cfg_ >"$masterserver_shadow_cfg"

	if [[ "$personality" == "master" ]]; then
		cp "$masterserver_master_cfg" "$masterserver_cfg"
		echo -n 'SFSM NEW' >"$masterserver_data_path/metadata.sfs"
	elif [[ "$personality" == "shadow" ]]; then
		cp "$masterserver_shadow_cfg" "$masterserver_cfg"
	else
		test_fail "Wrong personality $personality"
	fi

	saunafs_info_[master${masterserver_id}_shadow_cfg]=$masterserver_shadow_cfg
	saunafs_info_[master${masterserver_id}_master_cfg]=$masterserver_master_cfg
	saunafs_info_[master${masterserver_id}_cfg]=$masterserver_cfg
	saunafs_info_[master${masterserver_id}_data_path]=$masterserver_data_path
	saunafs_info_[master${masterserver_id}_matoml]=$masterserver_matoml_port
	saunafs_info_[master${masterserver_id}_matocl]=$masterserver_matocl_port
	saunafs_info_[master${masterserver_id}_matocs]=$masterserver_matocs_port
	saunafs_info_[master${masterserver_id}_matots]=$masterserver_matocs_port
}

create_sfsmetalogger_cfg_() {
	echo "WORKING_USER = $(id -nu)"
	echo "WORKING_GROUP = $(id -ng)"
	echo "DATA_PATH = ${saunafs_info_[master_data_path]}"
	echo "MASTER_HOST = $(get_ip_addr)"
	echo "MASTER_PORT = ${saunafs_info_[matoml]}"
	create_magic_debug_log_entry_ "sfsmetalogger"
	echo "${METALOGGER_EXTRA_CONFIG-}" | tr '|' '\n'
}

prepare_metalogger_() {
	create_sfsmetalogger_cfg_ >"$etcdir/sfsmetalogger.cfg"
	saunafs_info_[metalogger_cfg]="$etcdir/sfsmetalogger.cfg"
}

create_sfshdd_cfg_() {
	local n=$disks_per_chunkserver
	local zoned_prefix=""

	if [[ $use_zoned_disks && $use_ramdisk ]]; then
		zoned_prefix="zonefs:"

		local metadataPath
		local dataPath
		local devPath
		local firstDisk=$(echo "${chunkserver_id} * ${n}" | bc)
		local lastDisk=$(echo "${firstDisk} + ${n} - 1" | bc)

		for disk_number in $(seq -w ${firstDisk} ${lastDisk}); do
			if mount | grep "sauna_nullb${disk_number}" &>/dev/null; then
				sudo umount -l "/mnt/zoned/sauna_nullb${disk_number}" &>/dev/null
			fi

			create_zoned_nullb 4096 $zone_size_mb $number_of_conv_zones \
				$number_of_seq_zones ${disk_number}

			if [ -b /dev/sauna_nullb${disk_number} ]; then
				devPath="/dev/sauna_nullb${disk_number}"
			elif [ -b /dev/nullb${disk_number} ]; then
				devPath="/dev/nullb${disk_number}"
			fi

			sudo mkzonefs -f -o perm=666 "${devPath}" &>/dev/null

			metadataPath="/mnt/ramdisk/metadata/sauna_nullb${disk_number}"
			dataPath="/mnt/zoned/sauna_nullb${disk_number}"

			if [ ! -d "${metadataPath}" ]; then
				mkdir -pm 777 "${metadataPath}"
			fi

			# Mount point
			if [ ! -d "${dataPath}" ]; then
				sudo mkdir -pm 777 "${dataPath}"
			fi

			sudo mount -t zonefs "${devPath}" "${dataPath}"

			echo "${zoned_prefix}${metadataPath} | ${dataPath}"
		done
	elif [[ $use_ramdisk ]]; then
		local disk_number
		for ((disk_number = 0; disk_number < n; disk_number++)); do
			# Use path provided in env variable, if present generate some pathname otherwise.
			local this_disk_variable="CHUNKSERVER_${chunkserver_id}_DISK_${disk_number}"
			if [[ ${!this_disk_variable-} ]]; then
				local disk_dir=${!this_disk_variable}
			else
				local disk_dir=$RAMDISK_DIR/hdd_${chunkserver_id}_${disk_number}
			fi
			mkdir -pm 777 "${disk_dir}"
			echo "${zoned_prefix}${disk_dir}"
		done
	else
		for d in "${disks[@]:$((n * chunkserver_id)):$n}"; do
			echo "${zoned_prefix}${d}/meta | ${d}/data"
		done
	fi
}

# Creates LABEL entry for chunkserver's config from CHUNKSERVER_LABELS variable which is in a form:
# 0,1,2,3:hdd|4,5,6,7:ssd
# Usage: create_chunkserver_label_entry_ <chunkserver_id>
create_chunkserver_label_entry_() {
	local csid=$1
	tr '|' "\n" <<<"${CHUNKSERVER_LABELS-}" | awk -F: '$1~/(^|,)'$csid'(,|$)/ {print "LABEL = "$2}'
}

create_sfschunkserver_cfg_() {
	local this_module_cfg_variable="CHUNKSERVER_${chunkserver_id}_EXTRA_CONFIG"
	echo "SYSLOG_IDENT = chunkserver_${chunkserver_id}"
	echo "WORKING_USER = $(id -nu)"
	echo "WORKING_GROUP = $(id -ng)"
	echo "DATA_PATH = $chunkserver_data_path"
	echo "HDD_CONF_FILENAME = $hdd_cfg"
	echo "HDD_LEAVE_SPACE_DEFAULT = 128MiB"
	echo "MASTER_HOST = $ip_address"
	echo "MASTER_PORT = ${saunafs_info_[matocs]}"
	echo "CSSERV_LISTEN_PORT = $csserv_port"
	create_chunkserver_label_entry_ "${chunkserver_id}"
	create_magic_debug_log_entry_ "chunkserver_${chunkserver_id}"
	echo "${CHUNKSERVER_EXTRA_CONFIG-}" | tr '|' '\n'
	echo "${!this_module_cfg_variable-}" | tr '|' '\n'
}

add_chunkserver_() {
	local chunkserver_id=$1
	local csserv_port
	local chunkserver_data_path=$vardir/chunkserver_$chunkserver_id
	local hdd_cfg=$etcdir/sfshdd_$chunkserver_id.cfg
	local chunkserver_cfg=$etcdir/sfschunkserver_$chunkserver_id.cfg

	get_next_port_number csserv_port
	create_sfshdd_cfg_ >"$hdd_cfg"
	create_sfschunkserver_cfg_ >"$chunkserver_cfg"
	mkdir -p "$chunkserver_data_path"
	sfschunkserver -c "$chunkserver_cfg" start

	saunafs_info_[chunkserver${chunkserver_id}_port]=$csserv_port
	saunafs_info_[chunkserver${chunkserver_id}_cfg]=$chunkserver_cfg
	saunafs_info_[chunkserver${chunkserver_id}_hdd]=$hdd_cfg
}

create_sfsmount_cfg_() {
	local this_mount_cfg_variable="MOUNT_${1}_EXTRA_CONFIG"
	local this_mount_exports_variable="MOUNT_${1}_EXTRA_EXPORTS"
	echo "sfsmaster=$ip_address"
	echo "sfsport=${saunafs_info_[matocl]}"
	if [[ ${!this_mount_exports_variable-} ]]; then
		# we want custom exports options, so we need to identify with a password
		echo "sfspassword=${1}"
	fi
	echo "${MOUNT_EXTRA_CONFIG-}" | tr '|' '\n'
	echo "${!this_mount_cfg_variable-}" | tr '|' '\n'
}

do_mount_() {
	local mount_id=$1
	local workingdir=$TEMP_DIR/var/mount$mount_id
	# Make sure mount process is in a writable directory (for core dumps)
	mkdir -p $workingdir
	cd $workingdir
	for try in $(seq 1 $max_tries); do
		${saunafs_info_[mntcall${mount_id}]} && return 0
		echo "Retrying in 1 second ($try/$max_tries)..."
		sleep 1
	done
	echo "Cannot mount in $mount_dir, exiting"
	cd -
	exit 2
}

configure_mount_() {
	local mount_id=$1
	local mount_cmd=$2
	local mount_cfg=${saunafs_info_[mount${mount_id}_cfg]}
	local mount_dir=${saunafs_info_[mount${mount_id}]}
	local fuse_options=""
	for fuse_option in $(echo ${FUSE_EXTRA_CONFIG-} | tr '|' '\n'); do
		fuse_option_name=$(echo $fuse_option | cut -f1 -d'=')
		${mount_cmd} --help |& grep " -o ${fuse_option_name}[ =]" >/dev/null ||
			test_fail "Your libfuse doesn't support $fuse_option_name flag"
		fuse_options+="-o $fuse_option "
	done
	local call="${command_prefix} ${mount_cmd} -c ${mount_cfg} ${mount_dir} ${fuse_options}"
	saunafs_info_[mntcall$mount_id]=$call
	saunafs_info_[mnt${mount_id}_command]=${mount_cmd}
}

add_mount_() {
	local mount_id=$1
	local mount_dir=$mntdir/sfs${mount_id}
	local mount_cfg=$etcdir/sfsmount${mount_id}.cfg
	local sfsmount_available=false
	if $(which sfsmount &>/dev/null); then sfsmount_available=true; fi
	create_sfsmount_cfg_ ${mount_id} >"$mount_cfg"
	mkdir -p "$mount_dir"
	saunafs_info_[mount${mount_id}]="$mount_dir"
	saunafs_info_[mount${mount_id}_cfg]="$mount_cfg"
	max_tries=30

	if [ -z ${SAFS_MOUNT_COMMAND+x} ]; then
		if [ "$sfsmount_available" = true ]; then
			SAFS_MOUNT_COMMAND=sfsmount
			echo "Using libfuse3 for mounting filesystem."
		else
			echo "No sfsmount executable available, exiting"
			exit 2
		fi
	fi

	configure_mount_ ${mount_id} ${SAFS_MOUNT_COMMAND}
	do_mount_ ${mount_id}
}

add_cgi_server_() {
	local cgi_server_port
	local pidfile="$vardir/saunafs-cgiserver.pid"
	get_next_port_number cgi_server_port
	saunafs-cgiserver -P "$cgi_server_port" -p "$pidfile"
	saunafs_info_[cgi_pidfile]=$pidfile
	saunafs_info_[cgi_port]=$cgi_server_port
	saunafs_info_[cgi_url]="http://localhost:$cgi_server_port/sfs.cgi?masterport=${saunafs_info_[matocl]}"
}

# Some helper functions for tests to manipulate the existing installation

sfs_dir_info() {
	if (($# != 2)); then
		echo "Incorrect usage of saunafs dir_info with args $*"
		exit 2
	fi
	field=$1
	file=$2
	saunafs dirinfo "$file" | grep -w "$field" | grep -o '[0-9]*'
}

find_first_chunkserver_with_chunks_matching() {
	local pattern=$1
	local count=${saunafs_info_[chunkserver_count]}
	local chunkserver
	for ((chunkserver = 0; chunkserver < count; ++chunkserver)); do
		local hdds=$(get_metadata_path "${saunafs_info_[chunkserver${chunkserver}_hdd]}")
		if [[ $(find $hdds -type f -name "$pattern") ]]; then
			echo $chunkserver
			return 0
		fi
	done
	return 1
}

readonly chunk_metadata_extension=".met"
readonly chunk_data_extension=".dat"

get_metadata_path() {
	echo $(cat $1 | sed -e 's/*//' -e 's/zonefs://' | cut -d '|' -f 1)
}

get_data_path() {
	echo $(cat $1 | sed -e 's/*//' -e 's/zonefs://' | cut -d '|' -f 2)
}

# print absolute paths of all chunk files on selected server, one per line
find_chunkserver_chunks() {
	local chunkserver_number=$1
	local chunk_metadata_pattern="chunk*${chunk_metadata_extension}"
	local chunk_data_pattern="chunk*${chunk_data_extension}"
	shift
	local hdds=$(sed -e 's/*//' -e 's/zonefs://' -e 's/|//' \
		${saunafs_info_[chunkserver${chunkserver_number}_hdd]})
	if (($# > 0)); then
		find $hdds "(" -name "${chunk_data_pattern}" \
			-o -name "${chunk_metadata_pattern}" ")" -a "(" "$@" ")"
	else
		find $hdds "(" -name "${chunk_data_pattern}" \
			-o -name "${chunk_metadata_pattern}" ")"
	fi
}

# print absolute paths of all metadata chunk files on selected server, one per line
find_chunkserver_metadata_chunks() {
	local chunkserver_number=$1
	local chunk_metadata_pattern="chunk*${chunk_metadata_extension}"
	shift

	local hdds=$(sed -e 's/*//' -e 's/zonefs://' -e 's/|//' \
		${saunafs_info_[chunkserver${chunkserver_number}_hdd]})

	local -a extended_args=()
	if (($# > 0)); then
		extended_args+=(-a "(" "$@" ")")
	fi

	find $hdds "(" -name "${chunk_metadata_pattern}" ")" "${extended_args[@]}"
}

# print absolute paths of all chunk files on all servers used in test, one per line
find_all_chunks() {
	local count=${saunafs_info_[chunkserver_count]}
	local chunkserver
	for ((chunkserver = 0; chunkserver < count; ++chunkserver)); do
		find_chunkserver_chunks $chunkserver "$@"
	done
}

# print absolute paths of all metadata chunk files on all servers used in test, one per line
find_all_metadata_chunks() {
	local count=${saunafs_info_[chunkserver_count]}
	local chunkserver
	for ((chunkserver = 0; chunkserver < count; ++chunkserver)); do
		find_chunkserver_metadata_chunks $chunkserver "$@"
	done
}

# A useful shortcut for saunafs-admin
# Usage: saunafs_admin_master_no_password <command> [option...]
# Calls saunafs-admin with the given command and and automatically adds address
# of the master server
saunafs_admin_master_no_password() {
	local command="$1"
	shift
	saunafs-admin "$command" localhost "${saunafs_info_[matocl]}" --porcelain "$@"
}

# A useful shortcut for saunafs-admin commands which require authentication
# Usage: saunafs_admin_master <command> [option...]
# Calls saunafs-admin with the given command and and automatically adds address
# of the master server and authenticates
saunafs_admin_master() {
	local command="$1"
	shift
	local port=${saunafs_info_[matocl]}
	saunafs-admin "$command" localhost "$port" "$@" <<<"${saunafs_info_[admin_password]}"
}

# A useful shortcut for saunafs-admin commands which require authentication
# Usage: saunafs_admin_shadow <n> <command> [option...]
# Calls saunafs-admin with the given command and and automatically adds address
# of the n'th shadow master server and authenticates
saunafs_admin_shadow() {
	local id="$1"
	local command="$2"
	shift 2
	local port=${saunafs_info_[master${id}_matocl]}
	saunafs-admin "$command" localhost "$port" "$@" <<<"${saunafs_info_[admin_password]}"
}

# Stops the active master server without dumping metadata
saunafs_stop_master_without_saving_metadata() {
	saunafs_admin_master stop-master-without-saving-metadata
	assert_eventually "! sfsmaster -c ${saunafs_info_[master_cfg]} isalive"
}

# print the number of fully operational chunkservers
saunafs_ready_chunkservers_count() {
	saunafs-admin ready-chunkservers-count localhost ${saunafs_info_[matocl]}
}

# saunafs_wait_for_ready_chunkservers <num> -- waits until <num> chunkservers are fully operational
saunafs_wait_for_ready_chunkservers() {
	local chunkservers=$1
	local port=${saunafs_info_[matocl]}
	while [[ "$(saunafs-admin ready-chunkservers-count localhost $port 2>/dev/null | cat)" != "$chunkservers" ]]; do
		sleep 0.1
	done
}

saunafs_wait_for_all_ready_chunkservers() {
	saunafs_wait_for_ready_chunkservers ${saunafs_info_[chunkserver_count]}
}

# saunafs_shadow_synchronized <num> -- check if shadow <num> is fully synchronized with master
saunafs_shadow_synchronized() {
	local num=$1
	local port1=${saunafs_info_[matocl]}
	local port2=${saunafs_info_[master${num}_matocl]}
	local admin1="saunafs-admin metadataserver-status --porcelain localhost $port1"
	local admin2="saunafs-admin metadataserver-status --porcelain localhost $port2"
	if [[ "$($admin1 | cut -f3)" == "$($admin2 | cut -f3)" ]]; then
		return 0
	else
		return 1
	fi
}

# Prints number of chunks on each chunkserver in the following form:
# <ip1>:<port1>:<label> <chunks1>
# <ip2>:<port2>:<label> <chunks2>
# ...
saunafs_rebalancing_status() {
	saunafs_admin_master_no_password \
		list-chunkservers | sort | awk '$2 == "'$SAUNAFS_VERSION'" {print $1":"$10,$3}'
}

# Tells if the hdd configuration line is for a zoned disk
function is_zoned_device() {
	if [[ "${1}" = *zonefs* ]]; then
		echo true
	else
		echo false
	fi
}
