CHUNKSERVERS=1 \
	MASTERSERVERS=2 \
	USE_RAMDISK="YES" \
	MASTER_EXTRA_CONFIG="MAGIC_DISABLE_METADATA_DUMPS = 1" \
	setup_local_empty_saunafs info

assert_eventually_prints $'master\trunning' \
		"saunafs-probe metadataserver-status --porcelain localhost ${info[matocl]} | cut -f-2"

version=$(saunafs-probe metadataserver-status --porcelain localhost ${info[matocl]} | cut -f3)
# Version of last changelog entry.
changelog_version=$(tail -1 "${info[master_data_path]}"/changelog.sfs | grep -o '^[0-9]*')

# Make sure probe returned correct metadata version.
assert_equals $version "$((changelog_version + 1))"

saunafs_master_n 1 start

assert_eventually_prints $'shadow\tconnected\t'$version \
		"saunafs-probe metadataserver-status --porcelain localhost ${info[master1_matocl]}"

saunafs_master_n 0 stop

assert_eventually_prints $'shadow\tdisconnected\t'$version \
		"saunafs-probe metadataserver-status --porcelain localhost ${info[master1_matocl]}"
