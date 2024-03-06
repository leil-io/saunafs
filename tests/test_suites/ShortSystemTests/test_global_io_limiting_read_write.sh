timeout_set 1 minute

# Create a config file with a limit of 1 MB/s for all processes
iolimits="$TEMP_DIR/iolimits.cfg"
echo "limit unclassified 1024" > "$iolimits"

E=11
if is_windows_system; then
	E=15
fi
if valgrind_enabled; then
	E=$((5 * E))
fi

CHUNKSERVERS=3 \
	MOUNTS=2 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="GLOBALIOLIMITS_FILENAME = $iolimits" \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

if is_windows_system; then
	FILE_SIZE=1048576 file-generate "${info[mount0]}/read"
else
	truncate -s1M "${info[mount0]}/read"
fi

# consume accumulated limit
cat "${info[mount0]}/read" >/dev/null

start=$(nanostamp)
cat "${info[mount0]}/read" >/dev/null &
head -c1M /dev/zero >"${info[mount1]}/write" &
wait
end=$(nanostamp)

expected=$((2 * 1000 * 1000 * 1000))
time=$((end - start))
abserr=$((time - expected))
relerr=$((100 * abserr / expected))
echo $expected $time $abserr $relerr
assert_near 0 $relerr $E
