CHUNKSERVERS=4 \
	SFSEXPORTS_EXTRA_OPTIONS="allcanchangequota" \
	# MASTER_CUSTOM_GOALS="2 2: _" \
	USE_RAMDISK=YES \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

touch test
mkdir dir

goal=$(sfsgetgoal dir)

assert_equals "$goal" "dir: 1"

goal=$(sfssetgoal 2 dir)

assert_equals "$goal" "dir: 2"


time=$(sfsgettrashtime test)
assert_equals "$time" "test: 86400"
sfssettrashtime 0 test
time=$(sfsgettrashtime test)
assert_equals "$time" "test: 0"

attrs=$(sfsgeteattr test)
assert_equals "$attrs" "test: -"
sfsseteattr -f noattrcache test
attrs=$(sfsgeteattr test)
assert_equals "$attrs" "test: noattrcache"
sfsdeleattr -f noattrcache test
attrs=$(sfsgeteattr test)
assert_equals "$attrs" "test: -"

check=$(sfscheckfile test)
assert_equals "$check" "test:"
check=$(sfsfileinfo test)
assert_equals "$check" "test:"

echo "foo" > test
echo "bar" > test2

sfsappendchunks test2 test
# We aren't checking the result, since it can take some time. Another test
# should be check if it's working with (with the saunafs command)

assert_equals "$(sfsdirinfo dir)" "$(saunafs dirinfo dir)"

sfsfilerepair test

sfsmakesnapshot dir dir_snapshot

quota=$(sfsrepquota -d dir)
assert_equals "$quota" "# User/Group ID/Directory; Bytes: current usage, soft limit, hard limit; Inodes: current usage, soft limit, hard limit;"

sfssetquota -d 1000000 2000 1000000 2000 dir

quota=$(sfsrepquota -d dir)
assert_equals "$quota" "$(saunafs repquota -d dir)"
