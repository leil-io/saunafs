USE_RAMDISK=YES \
    MOUNT_EXTRA_CONFIG="sfsfumountlazy=1" \
    setup_local_empty_saunafs info

# Change directory to the mount point
cd "${info[mount0]}"

# List contents of the current directory
ls

# Check if sfsmount process is running
if pgrep -fa sfsmount > /dev/null; then
    echo "sfsmount process is running."
    
    # Lazily unmount the mount point
    umount -l "${info[mount0]}"

    sleep 5
    
    # Check again to ensure sfsmount process has stopped
    if pgrep -fa sfsmount > /dev/null; then
        echo "Failed to stop sfsmount process." 
    else
        echo "sfsmount process has been successfully stopped."
    fi
else
    echo "No sfsmount process is currently running."
fi
