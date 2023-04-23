build_xaunafs_or_use_cache() {
	local patch_path="${SOURCE_DIR}"/tests/tools/xaunafs_valgrind.patch

	# Exit if XaunaFS was already configured and installed,
	# assume it was configured properly
	(cd "$XAUNAFS_DIR/src/sfs-1.6.27" && make install) && return || true

	rm -rf "$XAUNAFS_DIR"
	mkdir -p "$XAUNAFS_DIR"
	pushd "$XAUNAFS_DIR"
	mkdir src
	cd src
	wget http://xaunafs.org/tl_files/sfscode/sfs-1.6.27-5.tar.gz
	tar xzf sfs-1.6.27-5.tar.gz
	cd sfs-1.6.27
	patch -p1 < $patch_path
	./configure --prefix="$XAUNAFS_DIR"
	make install
	popd
}

test_xaunafs() {
	test -x "$XAUNAFS_DIR/bin/sfsmount"
	test -x "$XAUNAFS_DIR/sbin/sfschunkserver"
	test -x "$XAUNAFS_DIR/sbin/sfsmaster"
}

build_xaunafs() {
	build_xaunafs_or_use_cache
	test_xaunafs
}

xaunafs_chunkserver_daemon() {
	"$XAUNAFS_DIR/sbin/sfschunkserver" -c "${saunafs_info_[chunkserver$1_cfg]}" "$2" | cat
	return ${PIPESTATUS[0]}
}

xaunafs_master_daemon() {
	"$XAUNAFS_DIR/sbin/sfsmaster" -c "${saunafs_info_[master_cfg]}" "$1" | cat
	return ${PIPESTATUS[0]}
}

# A generic function to run XaunaFS commands. Usage examples:
# sfs sfssetgoal 3 file
# sfs sfsdirinfo file
# sfs sfsmetalogger stop
sfs() {
	local command="$1"
	shift
	"$XAUNAFS_DIR/"*bin"/$command" "$@" | cat
	return ${PIPESTATUS[0]}
}
