# This test does a very basic estimation for write and read speed of the system.
# The idea is to use it to compare the performance after some changes.

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="mfscachemode=NEVER" \
	CHUNKSERVER_EXTRA_CONFIG="GARBAGE_COLLECTION_FREQ_MS = 0|HDD_TEST_FREQ = 100000" \
	AUTO_SHADOW_MASTER="NO" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

sizeFittingRamdisk=1536

# Create a file and store the dd output
dd if=/dev/zero of=file bs=1M count=${sizeFittingRamdisk} oflag=direct \
	&> "${TEMP_DIR}/dd_write.log"

drop_caches

# Read the file and store the dd output
dd if=file of=/dev/null bs=1M iflag=direct &> "${TEMP_DIR}/dd_read.log"

echo "Write: $(grep copied ${TEMP_DIR}/dd_write.log | awk '{print $10, $11}')"
echo " Read: $(grep copied ${TEMP_DIR}/dd_read.log | awk '{print $10, $11}')"
