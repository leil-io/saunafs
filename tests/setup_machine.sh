#!/usr/bin/env bash
set -eux -o pipefail
readonly self="$(readlink -f "${BASH_SOURCE[0]}")"
readonly script_name="$(basename "${self}")"

usage() {
	cat >&2 <<-EOF
		Usage: ${script_name} setup hdd...

		This scripts prepares the machine to run SaunaFS tests here.
		Specifically:
		* creates users saunafstest, saunafstest_0, ..., saunafstest_9
		* adds all saunafstest users to the fuse group
		* grants all users rights to run programs as saunafstest users
		* grants all users rights to run 'pkill -9 -u <some saunafstest user>'
		* allows all users to mount fuse filesystem with allow_other option
		* creates a 2G ramdisk in /mnt/ramdisk
		* creates 6 files mounted using loop device

		Example:
		${script_name} setup /mnt
		${script_name} setup /mnt/hda /mnt/hdb

		You need root permissions to run this script
	EOF
	exit 1
}

tool_exists() {
	type -P -v "${1}" >/dev/null
}

minimum_number_of_args=2
if [[ ( ( "${1}" != "setup" ) && ( "${1}" != "setup-force" ) ) || ( ${#} -lt ${minimum_number_of_args} ) ]]; then
	usage >&2
fi

if grep -q 'saunafstest_loop' /etc/fstab; then
	if [[ "${1}" != "setup-force" ]]; then
		echo 'The machine is at least partialy configured'
		echo 'Run revert-setup-machine.sh to revert the current configuration'
		exit 1
	fi
fi

shift
umask 0022

echo ; echo 'Install necessary programs'
# lsb_release is required by both build scripts and this script -- install it first
if ! command -v  lsb_release >/dev/null; then
	if command -v dnf >/dev/null; then
		dnf -y install redhat-lsb-core
	elif command -v  yum >/dev/null; then
		yum -y install redhat-lsb-core
	elif command -v  apt-get >/dev/null; then
		apt-get -y install lsb-release
	fi
fi
common_packages=(
	acl
	asciidoc
	attr
	automake
	bash-completion
	bc
	ccache
	cmake
	curl
	dbench
	debhelper
	devscripts
	fakeroot
	fio
	fuse3
	gcc
	git
	gnupg2
	kmod
	lcov
	make
	nfs4-acl-tools
	pkg-config
	pylint
	python3-pip
	python3-setuptools
	python3-wheel
	psmisc
	rsync
	rsyslog
	socat
	sudo
	tidy
	time
	valgrind
	wget
	## For NFS-Ganesha tests (duplicate are commented out as reference)
	# acl
	# asciidoc
	# cmake
	# fio
	bison
	byacc
	ceph
	dbus
	doxygen
	flex
	tree
)
apt_packages=(
	build-essential
	libblkid-dev
	libboost-filesystem-dev
	libboost-iostreams-dev
	libboost-program-options-dev
	libboost-system-dev
	libcrcutil-dev
	libdb-dev
	libfmt-dev
	libfuse3-dev
	libgoogle-perftools-dev
	libgtest-dev
	libisal-dev
	libjudy-dev
	libpam0g-dev
	libspdlog-dev
	libsystemd-dev
	libthrift-dev
	libtirpc-dev
	liburcu-dev
	libyaml-cpp-dev
	netcat-openbsd
	python3-venv
	uuid-dev
	zlib1g-dev
	## For NFS-Ganesha tests (duplicate are commented out as reference)
	# build-essential
	# libblkid-dev
	# libboost-filesystem-dev
	# libboost-iostreams-dev
	# libboost-program-options-dev
	# libboost-system-dev
	# libjudy-dev
	# liburcu-dev
	docbook
	docbook-xml
	krb5-user
	libacl1-dev
	libcap-dev
	libcephfs-dev
	libdbus-1-dev
	libglusterfs-dev
	libgssapi-krb5-2
	libjemalloc-dev
	libkrb5-dev
	libkrb5support0
	libnfsidmap-dev
	libradospp-dev
	libradosstriper-dev
	librgw-dev
	libsqlite3-dev
	xfslibs-dev
)
dnf_packages=(
	boost-filesystem
	boost-iostreams
	boost-program-options
	boost-system
	dnf-utils
	fmt-devel
	fuse3-devel
	gcc-c++
	gperftools-libs
	gtest-devel
	isa-l-devel
	Judy-devel
	kernel-devel
	libblkid-devel
	libcutl-devel
	libdb-devel
	libtirpc-devel
	netcat
	pam-devel
	pkgconfig
	python3-virtualenv
	rpm-build
	spdlog-devel
	systemd-devel
	thrift-devel
	userspace-rcu-devel
	uuid-devel
	yaml-cpp-devel
	zlib
	zlib-devel
	## For NFS-Ganesha tests (duplicate are commented out as reference)
	# boost-filesystem
	# boost-iostreams
	# boost-program-options
	# boost-system
	# gcc-c++
	# Judy-devel
	# kernel-devel
	# userspace-rcu-devel
	dbus-devel
	docbook-dtds
	docbook-style-xsl
	jemalloc-devel
	krb5-libs
	krb5-workstation
	libacl-devel
	libcap-devel
	libcephfs-devel
	libglusterfs-devel
	libnfsidmap-devel
	librados-devel
	libradosstriper-devel
	librgw-devel
	libsqlite3x-devel
	xfsprogs-devel
)
python_packages=(
	asciidoc
	black
	devscripts
	flask
	lcov_cobertura
	mypy
	pdfminer.six
	requests
	types-requests
)
# determine which OS we are running and choose the right set of packages to be installed
release="$(lsb_release -si)/$(lsb_release -sr)"
case "${release}" in
	LinuxMint/*|Ubuntu/*|Debian/*)
		apt-get -y install ca-certificates-java # https://www.mail-archive.com/debian-bugs-dist@lists.debian.org/msg1911078.html
		apt-get -y install "${common_packages[@]}" "${apt_packages[@]}"
		;;
	Fedora/*)
		dnf -y install "${common_packages[@]}" "${dnf_packages[@]}"
		update-alternatives --remove-all nc
		update-alternatives --install /usr/bin/nc nc /usr/bin/netcat 1
		;;
	*)
		set +x
		echo "Installation of required packages SKIPPED, '${release}' isn't supported by this script"
		;;
esac
gpg2 --list-keys
#setup gtest from sources
: "${GTEST_ROOT:=/usr/local}"
readonly gtest_temp_build_dir="$(mktemp -d)"
cmake -S /usr/src/googletest -B "${gtest_temp_build_dir}" -DCMAKE_INSTALL_PREFIX="${GTEST_ROOT}"
make -C "${gtest_temp_build_dir}" install
rm -rf "${gtest_temp_build_dir:?}"
# Setup python3 with a global virtual env (new recommended way)
VIRTUAL_ENV=/var/lib/saunafs_setup_machine_venv
export VIRTUAL_ENV
sed '\@VIRTUAL_ENV="'"${VIRTUAL_ENV}"'"@!s@$@\nVIRTUAL_ENV="'"${VIRTUAL_ENV}"'"@' -zi /etc/environment
# shellcheck disable=SC2016
sed '\@PATH="'"${VIRTUAL_ENV}"'/bin@!s@$@\nPATH="'"${VIRTUAL_ENV}"'/bin:${PATH}"@' -zi /etc/environment
python3 -m venv "${VIRTUAL_ENV}"
"${VIRTUAL_ENV}/bin/python3" -m pip install install "${python_packages[@]}"

echo ; echo 'Add group fuse'
groupadd -f fuse

echo ; echo 'Add user saunafstest'
if ! getent passwd saunafstest > /dev/null; then
	useradd --system --user-group --home /var/lib/saunafstest --create-home saunafstest
	chmod 755 /var/lib/saunafstest
fi

if ! groups saunafstest | grep -w fuse > /dev/null; then
	usermod -a -G fuse saunafstest # allow this user to mount fuse filesystem
fi
if ! groups saunafstest | grep -w adm > /dev/null; then
	usermod -a -G adm saunafstest # allow this user to read files from /var/log
fi

echo ; echo 'Prepare sudo configuration'
if ! [[ -d /etc/sudoers.d ]]; then
	# Sudo is not installed by default on Debian machines
	echo "sudo not installed!" >&2
	echo "Install it manually: apt-get install sudo" >&2
	echo "Then run this script again" >&2
	exit 1
fi
if ! [[ -f /etc/sudoers.d/saunafstest ]] || \
		! grep drop_caches /etc/sudoers.d/saunafstest >/dev/null; then
	cat >/etc/sudoers.d/saunafstest <<-END
		Defaults rlimit_core=default
		ALL ALL = (saunafstest) NOPASSWD: ALL
		ALL ALL = NOPASSWD: /usr/bin/pkill -9 -u saunafstest
		ALL ALL = NOPASSWD: /bin/rm -rf /tmp/saunafs_error_dir
		saunafstest ALL = NOPASSWD: /bin/sh -c echo\ 1\ >\ /proc/sys/vm/drop_caches
	END
	chmod 0440 /etc/sudoers.d/saunafstest
fi
if ! [[ -d /etc/security/limits.d ]]; then
	echo "pam module pam_limits is not installed!" >&2
	exit 1
fi
if ! [[ -f /etc/security/limits.d/10-saunafstests.conf ]]; then
	# Change limit of open files for user saunafstest
	echo "saunafstest hard nofile 10000" > /etc/security/limits.d/10-saunafstests.conf
	chmod 0644 /etc/security/limits.d/10-saunafstests.conf
fi
if ! grep 'pam_limits.so' /etc/pam.d/sudo > /dev/null; then
	cat >>/etc/pam.d/sudo <<-END
		### Reload pam limits on sudo - necessary for saunafs tests. ###
		session required pam_limits.so
	END
fi

case "${release}" in
	LinuxMint/*|Ubuntu/*|Debian/*)
		echo ; echo 'Configure SaunaFS repository'
		gpg --no-default-keyring \
			--keyring /usr/share/keyrings/saunafs-archive-keyring.gpg \
			--keyserver hkps://keyserver.ubuntu.com \
			--receive-keys 0xA80B96E2C79457D4
		sudo tee /etc/apt/sources.list.d/saunafs.list <<-EOF
		deb [arch=amd64 signed-by=/usr/share/keyrings/saunafs-archive-keyring.gpg] https://repo.saunafs.com/repository/saunafs-$(lsb_release -si | tr '[:upper:]' '[:lower:]')-$(lsb_release -sr | tr '/' '-') $(lsb_release -sc) main
		EOF
		;;
	*)
		set +x
		echo "Configuration of packages repository SKIPPED, '${release}' isn't supported by this script"
		;;
esac

echo ; echo 'Add users saunafstest_{0..9}'
for username in saunafstest_{0..9}; do
	if ! getent passwd ${username} > /dev/null; then
		useradd --system --user-group --home /var/lib/${username} --create-home \
				--groups fuse,saunafstest ${username}
		cat >>/etc/sudoers.d/saunafstest <<-END

			ALL ALL = (${username}) NOPASSWD: ALL
			ALL ALL = NOPASSWD: /usr/bin/pkill -9 -u ${username}
		END
	fi
done

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# SMR' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# SMR
		saunafstest ALL = NOPASSWD: /usr/sbin/modprobe null_blk nr_devices=0
		saunafstest ALL = NOPASSWD: /usr/sbin/mkzonefs -f -o perm=666 /dev/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/sbin/mkzonefs -f -o perm=666 /dev/nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/mount -t zonefs /dev/sauna_nullb* /mnt/zoned/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/mount -t zonefs /dev/nullb* /mnt/zoned/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/umount -l /mnt/zoned/sauna_nullb*

		saunafstest ALL = NOPASSWD: /usr/bin/mkdir -pm 777 /mnt/zoned/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/mkdir /sys/kernel/config/nullb/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/rmdir /sys/kernel/config/nullb/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/tee /sys/kernel/config/nullb/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/tee /sys/block/sauna_nullb*
		saunafstest ALL = NOPASSWD: /usr/bin/tee /sys/block/nullb*
	END
fi

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# Ganesha' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# Ganesha automated tests
		saunafstest ALL = NOPASSWD: /tmp/SaunaFS-autotests/mnt/sfs0/bin/ganesha.nfsd
		saunafstest ALL = NOPASSWD: /usr/bin/ganesha.nfsd
		saunafstest ALL = NOPASSWD: /usr/bin/pkill -9 ganesha.nfsd
		saunafstest ALL = NOPASSWD: /usr/bin/mkdir -p /var/run/ganesha
		saunafstest ALL = NOPASSWD: /usr/bin/touch /var/run/ganesha/ganesha.pid
		saunafstest ALL = NOPASSWD: /usr/bin/mount, /usr/bin/umount
	END
fi

if [ ! -f /etc/sudoers.d/saunafstest ] || ! grep -q '# Client' /etc/sudoers.d/saunafstest >/dev/null; then
	cat <<-'END' >>/etc/sudoers.d/saunafstest
		# Client
		saunafstest ALL = NOPASSWD: /usr/bin/tee /tmp/SaunaFS-autotests/mnt/sfs*
	END
fi

echo ; echo 'Fixing GIDs of users'
for name in saunafstest saunafstest_{0..9}; do
	uid=$(getent passwd "${name}" | cut -d: -f3)
	gid=$(getent group  "${name}" | cut -d: -f3)
	if [[ ${uid} == "" || ${gid} == "" ]]; then
		exit 1
	fi
	if [[ "${uid}" == "${gid}" ]]; then
		# UID is equal to GID -- we have to change it to something different,
		# so find some other free gid
		new_gid=$((gid * 2))
		while getent group ${new_gid}; do
			new_gid=$((new_gid + 1))
		done
		groupmod -g ${new_gid} ${name}
	fi
done

echo ; echo 'Prepare /etc/fuse.conf'
if ! grep '^[[:blank:]]*user_allow_other' /etc/fuse.conf >/dev/null; then
	echo "user_allow_other" >> /etc/fuse.conf
fi

echo ; echo 'Configure local port range for outbond connections'
if [ ! -f  /etc/sysctl.d/60-ip_port_range.conf ]; then
	echo "net.ipv4.ip_local_port_range=10000 64000" > /etc/sysctl.d/60-ip_port_range.conf
fi

echo ; echo 'Prepare empty /etc/saunafs_tests.conf'
if ! [[ -f /etc/saunafs_tests.conf ]]; then
	cat >/etc/saunafs_tests.conf <<-END
		: \${SAUNAFS_DISKS:="$*"}
		# : \${TEMP_DIR:=/tmp/SaunaFS-autotests}
		# : \${SAUNAFS_ROOT:=${HOME}/local}
		# : \${FIRST_PORT_TO_USE:=25000}
	END
fi

echo ; echo 'Prepare ramdisk'
if ! grep /mnt/ramdisk /etc/fstab >/dev/null; then
	echo "# Ramdisk used in SaunaFS tests" >> /etc/fstab
	echo "ramdisk  /mnt/ramdisk  tmpfs  mode=1777,size=2g" >> /etc/fstab
	mkdir -p /mnt/ramdisk
	mount /mnt/ramdisk
	# shellcheck disable=SC2016
	echo ': ${RAMDISK_DIR:=/mnt/ramdisk}' >> /etc/saunafs_tests.conf
fi

echo ; echo 'Prepare loop devices'
#creating loop devices more or less evenly distributed between available disks
i=0
devices=6
loops=()
while [ ${i} -lt ${devices} ] ; do
	for disk in "$@"; do
		if (( i == devices )); then #stop if we have enough devices
			break
		fi
		loops+=("/mnt/saunafstest_loop_${i}")
		if grep -q "saunafstest_loop_${i}" /etc/fstab; then
			(( ++i ))
			continue
		fi
		mkdir -p "${disk}/saunafstest_images"
		# Create image file
		image="${disk}/saunafstest_images/image_${i}"
		truncate -s 1G "${image}"
		mkfs.ext4 -Fq "${image}"
		# Add it to fstab
		echo "$(readlink -m "${image}") /mnt/saunafstest_loop_${i}  ext4  loop" >> /etc/fstab
		mkdir -p "/mnt/saunafstest_loop_${i}"
		# Mount and set permissions
		mount "/mnt/saunafstest_loop_${i}"
		chmod 1777 "/mnt/saunafstest_loop_${i}"
		(( ++i ))
	done
done
# shellcheck disable=SC2016
echo ': ${SAUNAFS_LOOP_DISKS:="'"${loops[*]}"'"}' >> /etc/saunafs_tests.conf

set +x
echo 'Machine configured successfully'
