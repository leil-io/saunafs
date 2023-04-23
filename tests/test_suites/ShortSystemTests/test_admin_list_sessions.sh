MOUNTS=2 \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"
echo "Hello World!" > test
saunafs-admin list-sessions localhost "${info[matocl]}"
sessions=$(saunafs-admin list-sessions localhost "${info[matocl]}")
num_files=$(grep "Open files: " <<< ${sessions} | sed 's/^.*: //' )
num_sessions=$(wc -l <<< ${num_files})
num_files=$(tr '\n' ' ' <<< ${num_files})

echo "Number of open files: ${num_files}"
echo "Number of sessions: ${num_sessions}"

expect_equals "2" ${num_sessions}
expect_equals "0 1 " "${num_files}"
