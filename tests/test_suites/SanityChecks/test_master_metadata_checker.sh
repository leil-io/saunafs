timeout_set 3 minutes
master_cfg="MAGIC_DISABLE_METADATA_DUMPS = 1"
master_cfg+="|MAGIC_DEBUG_LOG = $TEMP_DIR/syslog|LOG_FLUSH_ON=DEBUG"
touch "$TEMP_DIR/syslog"

CHUNKSERVERS=0 \
USE_RAMDISK="YES" \
ADMIN_PASSWORD="password" \
MASTER_EXTRA_CONFIG="$master_cfg" \
setup_local_empty_saunafs info

# Metadata size after initialize from empty metadata
METADATA_INIT_SIZE=296

function check_version_increased() {
    local version1=$1
    local version2=$2
    if [[ $version1 -gt $version2 ]]; then
        return 0
    fi
    return 1
}

metadata_init_version="$(metadata_get_version "${info[master_data_path]}/metadata.sfs")"

log_data="$(sed -n '/connected to Master/,$p' "${TEMP_DIR}/syslog" | grep -E info)"
metadata_path="$(echo ${log_data} | grep -E "*opened metadata file*"| awk -F 'metadata file ' '{print $2}' | awk '{print $1}')"
assert_file_exists "${metadata_path}"
assert_equals ${METADATA_INIT_SIZE} "$(stat -c %s "${metadata_path}")"
assert_equals 1 $(metadata_get_version "${info[master_data_path]}/metadata.sfs")

read -r total_inodes dir_inodes file_inodes symlink_inodes chunk_amount <<< "$(echo "${log_data}" | grep -E "metadata file ${metadata_path} read" | sed -n 's/.*metadata file .* read (\(.*\) inodes including \(.*\) directory inodes, \(.*\) file inodes, \(.*\) symlink inodes and \(.*\) chunks).*/\1 \2 \3 \4 \5 /p')"

cd "${info[mount0]}"

METADATA_INODES=10
# Create 10 directories at root
mkdir -p "${info[mount0]}"/dir{1..10}

# Create a file inside each directory
for dir in {1..10}; do
    touch "${info[mount0]}"/dir${dir}/file${dir}
done
# Create a symlink for each file at root
for file in {1..10}; do
    ln -s "${info[mount0]}"/dir${file}/file${file} "${info[mount0]}"/symlink${file}
done
saunafs-admin save-metadata localhost "${saunafs_info_[matocl]}" <<< "password"

saunafs_info_output=$(saunafs-admin info localhost "${saunafs_info_[matocl]}")
total_inodes_after_reload="$(grep -E "FS objects:" <<< "$saunafs_info_output" | awk '{print $3}')"
dir_inodes_after_reload="$(grep -E "Directories:" <<< "$saunafs_info_output" | awk '{print $2}')"
file_inodes_after_reload="$(grep -E "Files:" <<< "$saunafs_info_output" | awk '{print $2}')"
symlink_inodes_after_reload="$(grep -E "Symlinks:" <<< "$saunafs_info_output" | awk '{print $2}')"

# Total Directory inodes are 10 plus the root directory
assert_equals $((${METADATA_INODES} + 1)) ${dir_inodes_after_reload}
assert_equals ${METADATA_INODES} ${file_inodes_after_reload}
assert_equals ${METADATA_INODES} ${symlink_inodes_after_reload}
assert_equals $((${METADATA_INODES} * 3 + 1)) ${total_inodes_after_reload}

#check version has increased
metadata_current_version="$(metadata_get_version "${info[master_data_path]}/metadata.sfs")"
assert_success check_version_increased ${metadata_current_version} ${metadata_init_version} 

MASTER_RESTART_DELAY_SECS=3
sleep ${MASTER_RESTART_DELAY_SECS}
assert_success saunafs_master_daemon restart

log_data_after_reload="$(sed -n '/terminate signal received/,$p' "${TEMP_DIR}/syslog" | grep -E info)"
metadata_path_after_restart="$(echo ${log_data_after_reload} | grep -E "*opened metadata file*"| awk -F 'metadata file ' '{print $2}' | awk '{print $1}')"
assert_file_exists "${metadata_path_after_restart}"
read -r total_inodes dir_inodes file_inodes symlink_inodes chunk_amount <<< "$(echo "${log_data_after_reload}" | grep -E "metadata file ${metadata_path_after_restart} read" | sed -n 's/.*metadata file .* read (\(.*\) inodes including \(.*\) directory inodes, \(.*\) file inodes, \(.*\) symlink inodes and \(.*\) chunks).*/\1 \2 \3 \4 \5 /p')"

assert_equals ${dir_inodes_after_reload} ${dir_inodes}
assert_equals ${file_inodes_after_reload} ${file_inodes}
assert_equals ${symlink_inodes_after_reload} ${symlink_inodes}
assert_equals ${total_inodes_after_reload} ${total_inodes}
