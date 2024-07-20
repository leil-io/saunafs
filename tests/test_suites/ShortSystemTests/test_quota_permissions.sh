USE_RAMDISK=YES \
	MOUNT_EXTRA_CONFIG="mfscachemode=NEVER" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

gid1=$(id -g saunafstest_1)
uid1=$(id -u saunafstest_1)
gid=$(id -g saunafstest)
uid=$(id -u saunafstest)

expect_failure saunafs setquota -g $gid 0 0 3 6 .  # fail, permissions missing
expect_failure saunafs repquota -a .               # fail, permissions missing
expect_failure saunafs repquota -g $gid1 .         # fail, permissions missing
expect_failure saunafs repquota -u $uid1 .         # fail, permissions missing
expect_success saunafs repquota -g $gid .          # OK
expect_success saunafs repquota -u $uid .          # OK
