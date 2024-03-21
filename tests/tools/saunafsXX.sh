# Older (unsupported) versions of SaunaFS used in tests and sources of its packages
#
# TODO: Rename the functions to something better, like *saunafs_old
# TODO: Don't use build information in debian package versions. It causes
# problems when downgrading, because you need to include the build information
# in the downgrade command.
SAUNAFSXX_TAG_APT="4.0.1-20240301-164017-stable-main-82161d4f"
SAUNAFSXX_TAG="4.0.1"

install_saunafsXX() {
	rm -rf "$SAUNAFSXX_DIR"
	mkdir -p "$SAUNAFSXX_DIR"
	local distro="$(lsb_release -si)"
	case "$distro" in
	Ubuntu | Debian)
		local codename="$(lsb_release -sc)"
		mkdir -p ${TEMP_DIR}/apt/apt.conf.d
		mkdir -p ${TEMP_DIR}/apt/var/lib/apt/partial
		mkdir -p ${TEMP_DIR}/apt/var/cache/apt/archives/partial
		mkdir -p ${TEMP_DIR}/apt/var/lib/dpkg
		cp /var/lib/dpkg/status ${TEMP_DIR}/apt/var/lib/dpkg/status
		cat >${TEMP_DIR}/apt/apt.conf <<END
Dir::State "${TEMP_DIR}/apt/var/lib/apt";
Dir::State::status "${TEMP_DIR}/apt/var/lib/dpkg/status";
Dir::Etc::SourceList "${TEMP_DIR}/apt/saunafs.list";
Dir::Cache "${TEMP_DIR}/apt/var/cache/apt";
Dir::Etc::Parts "${TEMP_DIR}/apt/apt.conf.d";
END
		local destdir="${TEMP_DIR}/apt/var/cache/apt/archives"
		echo "deb [arch=amd64] https://repo.saunafs.com/repository/saunafs-ubuntu-22.04/ ${codename} main" >${TEMP_DIR}/apt/saunafs.list
		env APT_CONFIG="${TEMP_DIR}/apt/apt.conf" apt-get update
		env APT_CONFIG="${TEMP_DIR}/apt/apt.conf" apt-get -y --allow-downgrades install -d \
			saunafs-master=${SAUNAFSXX_TAG_APT} \
			saunafs-chunkserver=${SAUNAFSXX_TAG_APT} \
			saunafs-client=${SAUNAFSXX_TAG_APT} \
			saunafs-adm=${SAUNAFSXX_TAG_APT}
			# Not used right now
			# saunafs-metalogger=${SAUNAFSXX_TAG_APT}
		# unpack binaries
		cd ${destdir}
		find . -name "*master*.deb" | xargs dpkg-deb --fsys-tarfile | tar -x ./usr/sbin/sfsmaster
		find . -name "*chunkserver*.deb" | xargs dpkg-deb --fsys-tarfile | tar -x ./usr/sbin/sfschunkserver
		find . -name "*client*.deb" | xargs dpkg-deb --fsys-tarfile | tar -x ./usr/bin/
		find . -name "*adm*.deb" | xargs dpkg-deb --fsys-tarfile | tar -x ./usr/bin/
		# find . -name "*metalogger*.deb" | xargs dpkg-deb --fsys-tarfile | tar -x ./usr/sbin/
		cp -Rp usr/ ${SAUNAFSXX_DIR_BASE}/install
		cd -
		;;
#  We don't support testing CentOS and Fedora anymore, but the code is left
#  here for reference.
#
# 	CentOS | Fedora)
# 		local destdir="${TEMP_DIR}/saunafsxx_packages"
# 		mkdir ${destdir}
# 		local url=""
# 		if [ "$distro" == CentOS ]; then
# 			url="http://dev.saunafs.com/packages/centos.saunafs.repo"
# 		else
# 			url="http://dev.saunafs.com/packages/fedora.saunafs.repo"
# 		fi
# 		mkdir -p ${TEMP_DIR}/dnf/etc/yum.repos.d
# 		cat >${TEMP_DIR}/dnf/dnf.conf <<END
# [main]
# logdir=${TEMP_DIR}/dnf/var/log
# cachedir=${TEMP_DIR}/dnf/var/cache
# persistdir=${TEMP_DIR}/dnf/var/lib/dnf
# reposdir=${TEMP_DIR}/dnf/etc/yum.repos.d
# END
# 		wget "$url" -O ${TEMP_DIR}/dnf/etc/yum.repos.d/saunafs.repo
# 		for pkg in {saunafs-master,saunafs-chunkserver,saunafs-client}-${SAUNAFSXX_TAG}; do
# 			fakeroot dnf -y --config=${TEMP_DIR}/dnf/dnf.conf --destdir=${destdir} download ${pkg}
# 		done
# 		# unpack binaries
# 		cd ${destdir}
# 		find . -name "*master*.rpm" | xargs rpm2cpio | cpio -idm ./usr/sbin/sfsmaster
# 		find . -name "*chunkserver*.rpm" | xargs rpm2cpio | cpio -idm ./usr/sbin/sfschunkserver
# 		find . -name "*client*.rpm" | xargs rpm2cpio | cpio -idm ./usr/bin/*
#
	*)
		test_fail "Your distribution ($distro) is not supported."
		;;
	esac
	test_saunafsXX_executables
	echo "Legacy SaunaFS packages installed."
}

test_saunafsXX_executables() {
	local awk_version_pattern="/$(sed 's/[.]/[.]/g' <<<$SAUNAFSXX_TAG)/"
	echo $($SAUNAFSXX_DIR/bin/sfsmount --version 2>&1)
	echo $($SAUNAFSXX_DIR/sbin/sfschunkserver -v)
	echo $($SAUNAFSXX_DIR/sbin/sfsmaster -v)
	test -x "$SAUNAFSXX_DIR/bin/sfsmount"
	test -x "$SAUNAFSXX_DIR/sbin/sfschunkserver"
	test -x "$SAUNAFSXX_DIR/sbin/sfsmaster"
	assert_awk_finds $awk_version_pattern "$($SAUNAFSXX_DIR/bin/sfsmount --version 2>&1)"
	assert_awk_finds $awk_version_pattern "$($SAUNAFSXX_DIR/sbin/sfschunkserver -v)"
	assert_awk_finds $awk_version_pattern "$($SAUNAFSXX_DIR/sbin/sfsmaster -v)"
}

saunafsXX_chunkserver_daemon() {
	"$SAUNAFSXX_DIR/sbin/sfschunkserver" -c "${saunafs_info_[chunkserver$1_cfg]}" "$2" | cat
	return ${PIPESTATUS[0]}
}

saunafsXX_master_daemon() {
	"$SAUNAFSXX_DIR/sbin/sfsmaster" -c "${saunafs_info_[master_cfg]}" "$1" | cat
	return ${PIPESTATUS[0]}
}

saunafsXX_shadow_daemon_n() {
	local id=$1
	shift
	if is_windows_system; then
		windows_server_aux "$SAUNAFSXX_DIR/sbin/sfsmaster -c ${saunafs_info_[master${id}_cfg]}" "$@"
	else
		"$SAUNAFSXX_DIR/sbin/sfsmaster" -c "${saunafs_info_[master${id}_cfg]}" "$@" | cat
	fi
	return ${PIPESTATUS[0]}
}

# A generic function to run SaunaFS commands.
#
# Usage examples:
#   sfs sfssetgoal 3 file
#   sfs sfsdirinfo file
#   sfs sfsmetalogger stop
saunafsXX() {
	local command="$1"
	shift
	"$SAUNAFSXX_DIR/"*bin"/$command" "$@" | cat
	return ${PIPESTATUS[0]}
}

assert_saunafsXX_services_count_equals() {
	local mas_expected="${1}"
	local cs_expected="${2}"
	local cli_expected="${3}"
	assert_equals "${mas_expected}" "$(saunafs_old_admin_master info | grep "${SAUNAFSXX_TAG}" | wc -l)"
	assert_equals "${cs_expected}" "$(saunafs_old_admin_master list-chunkservers | grep "${SAUNAFSXX_TAG}" | wc -l)"
	assert_equals "${cli_expected}" "$(saunafs_old_admin_master list-mounts | grep "${SAUNAFSXX_TAG}" | wc -l)"
}

assert_no_saunafsXX_services_active() {
	assert_saunafsXX_services_count_equals 0 0 0
}

# TODO: Add metalogger and other service support
