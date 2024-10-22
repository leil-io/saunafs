# Goal of test:
# Verify that combinations of different readahead parameters successfully manage to read a file in different patterns.
#
assert_program_installed fio

timeout_set 20 minutes

CHUNKSERVERS=5 \
	USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="sfscachemode=NEVER" \
	setup_local_empty_saunafs info

file_size=$(( 4 * SAUNAFS_CHUNK_SIZE ))
# Note this path can be changed to dump output to desired location
fio_output_dir=${TEMP_DIR}/fio_outputs
mkdir $fio_output_dir

cd "$TEMP_DIR"
assert_success saunafs_mount_unmount 0

current_iteration=0
mount_cfg=${info[mount0_cfg]}
for cacheexpirationtime in 0 1 1000; do
	echo "cacheexpirationtime=$cacheexpirationtime" >> $mount_cfg
	for readaheadmaxwindowsize in 4096 65536; do
		echo "readaheadmaxwindowsize=$readaheadmaxwindowsize" >> $mount_cfg
		for readworkers in 5 30 100; do
			echo "readworkers=$readworkers" >> $mount_cfg
			for maxreadaheadrequests in 0 1 5 10; do
				echo "maxreadaheadrequests=$maxreadaheadrequests" >> $mount_cfg

				sed -i '/^\s*$/d' $mount_cfg

				echo ""
				echo "Starting test with parameters: cacheexpirationtime=$cacheexpirationtime,readaheadmaxwindowsize=$readaheadmaxwindowsize,readworkers=$readworkers,maxreadaheadrequests=$maxreadaheadrequests"

				output_dir=$fio_output_dir/$current_iteration
				mkdir $output_dir
				saunafs_mount_start 0
				cd ${info[mount0]}

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------------------"
				echo "                                Single Sequential Read"
				echo "---------------------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/ssr.txt -group_reporting -rw=read -bs=1MB -size=$file_size -name=seqread -runtime=100

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------------------"
				echo "                                  Single Random Read"
				echo "---------------------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/srr.txt -group_reporting -rw=randread -bs=1MB -size=$file_size -name=randread -runtime=100

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------------------"
				echo "                                 Five Sequential Reads"
				echo "---------------------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/sr5.txt -group_reporting -rw=read -bs=1MB -size=$file_size -name=seqreads_5 -runtime=100 -numjobs=5

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------------------"
				echo "                                   Five Random Reads"
				echo "---------------------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/rr5.txt -group_reporting -rw=randread -bs=1MB -size=$file_size -name=randreads_5 -runtime=100 -numjobs=5

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------------------"
				echo "                   128 Slow Sequential Reads + Fast Sequential Read"
				echo "---------------------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/ssr128_fsr.txt -group_reporting -rw=read -name=slowseqreads_128 -bs=$(( $file_size / 4096 )) -size=$(( $file_size / 512 )) -thinktime=125ms -runtime=2 -numjobs=128 -name=fastseqread -bs=1MB -size=$file_size

				drop_caches
				echo ""
				echo "---------------------------------------------------------------------------------------------"
				echo "                     128 Slow Random Reads + Fast Sequential Read"
				echo "---------------------------------------------------------------------------------------------"
				assert_success fio -filename=file -direct=1 -output=$output_dir/srr128_fsr.txt -group_reporting -name=slowrandreads_128 -rw=randread -bs=$(( $file_size / 4096 )) -size=$(( $file_size / 512 )) -thinktime=125ms -runtime=2 -numjobs=128 -name=fastseqread -rw=read -bs=1MB -size=$file_size

				cd "$TEMP_DIR"
				assert_success saunafs_mount_unmount 0
				current_iteration=$(( $current_iteration + 1 ))

				sed -ie "s/maxreadaheadrequests=$maxreadaheadrequests//g" $mount_cfg
			done
			sed -ie "s/readworkers=$readworkers//g" $mount_cfg
		done
		sed -ie "s/readaheadmaxwindowsize=$readaheadmaxwindowsize//g" $mount_cfg
	done
	sed -ie "s/cacheexpirationtime=$cacheexpirationtime//g" $mount_cfg
done
