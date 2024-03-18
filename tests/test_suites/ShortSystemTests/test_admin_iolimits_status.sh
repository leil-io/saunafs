iolimits="$TEMP_DIR/iolimits.cfg"
printf "subsystem blkio\nlimit /aaa 203\nlimit unclassified 301\n" > "$iolimits"

status() {
	saunafs-admin iolimits-status --porcelain localhost "${info[matocl]}"
}

CHUNKSERVERS=1 \
	USE_RAMDISK=YES \
	MASTER_EXTRA_CONFIG="GLOBALIOLIMITS_FILENAME = $iolimits"`
			`"|GLOBALIOLIMITS_RENEGOTIATION_PERIOD_SECONDS = 0.15"`
			`"|GLOBALIOLIMITS_ACCUMULATE_MS = 30" \
	setup_local_empty_saunafs info

expect_equals "1 150.000 30 blkio
/aaa 203
unclassified 301" "$(status)"

printf "subsystem blkio\nlimit /aaa 203\nlimit /aaa/bbb 504\nlimit unclassified 301\n" > "$iolimits"
saunafs_admin_master reload-config
expect_equals "2 150.000 30 blkio
/aaa 203
/aaa/bbb 504
unclassified 301" "$(status)"

printf "" > "$iolimits"
saunafs_admin_master reload-config
expect_equals "" "$(status)"
