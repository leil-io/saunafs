MOUNTS=2 \
	USE_RAMDISK=YES \
	FUSE_EXTRA_CONFIG="cACHEexpIRAtiONtIME=2000" \
	FUSE_0_EXTRA_CONFIG="sfsIORetries=20`
			`|MaxReadahEAdReqUests=2`
			`|SFSChUnKSerVerWaVEREadtO=200" \
	FUSE_1_EXTRA_CONFIG="sfsIORetries=60`
			`|MaxReadahEAdReqUests=10`
			`|SFSChUnKSerVerWaVEREadtO=300" \
	setup_local_empty_saunafs info

cd "${info[mount0]}"

assert_equals $(cat .saunafs_tweaks | egrep CacheExpirationTime | awk '{print $2}') 2000
assert_equals $(cat .saunafs_tweaks | egrep WriteMaxRetries | awk '{print $2}') 20
assert_equals $(cat .saunafs_tweaks | egrep MaxReadaheadRequests | awk '{print $2}') 2
assert_equals $(cat .saunafs_tweaks | egrep ReadWaveTimeout | awk '{print $2}') 200

cd "${info[mount1]}"

assert_equals $(cat .saunafs_tweaks | egrep CacheExpirationTime | awk '{print $2}') 2000
assert_equals $(cat .saunafs_tweaks | egrep WriteMaxRetries | awk '{print $2}') 60
assert_equals $(cat .saunafs_tweaks | egrep MaxReadaheadRequests | awk '{print $2}') 10
assert_equals $(cat .saunafs_tweaks | egrep ReadWaveTimeout | awk '{print $2}') 300
