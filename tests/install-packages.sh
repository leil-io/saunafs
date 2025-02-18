#!/bin/bash
set -x

readonly self="$(readlink -f "${BASH_SOURCE[0]}")"
readonly script_dir="$(dirname "$self")"

echo 'Installing necessary programs'
# lsb_release is required by both build scripts and this script -- install it first
if ! command -v  lsb_release >/dev/null; then
	if command -v dnf >/dev/null; then
		dnf -y install redhat-lsb-core
	elif command -v  yum >/dev/null; then
		yum -y install redhat-lsb-core
	elif command -v  apt-get >/dev/null; then
		apt-get update
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
	clang
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
	python3-gssapi      # required by Ganesha -> pynfs suite
	python3-pip
	python3-ply         # required by Ganesha -> pynfs suite
	python3-setuptools
	python3-wheel
	psmisc
	pv
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
	dbus
	doxygen
	flex
	tree
)
apt_packages=(
	build-essential
	iproute2
	iozone3             # required by Ganesha -> iozone suite
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
	libdbus-1-dev
	libgssapi-krb5-2
	libjemalloc-dev
	libkrb5-dev
	libkrb5support0
	libnfsidmap-dev
	libnsl-dev
	libsqlite3-dev
	software-properties-common
)
noble_packages=(
	prometheus-cpp-dev
	util-linux-extra
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
	iproute
	iozone              # required by Ganesha -> iozone suite
	isa-l-devel
	Judy-devel
	kernel-devel
	libblkid-devel
	libcutl-devel
	libdb-devel
	libnsl
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
	libnfsidmap-devel
	libprometheus-cpp-devel
	libsqlite3x-devel
	xfsprogs-devel
)

# determine which OS we are running and choose the right set of packages to be installed
release="$(lsb_release -si)/$(lsb_release -sr)"
case "${release}" in
	Ubuntu/24.04)
		echo $release
		apt-get -y install ca-certificates-java # https://www.mail-archive.com/debian-bugs-dist@lists.debian.org/msg1911078.html
		apt-get -y install "${common_packages[@]}" "${apt_packages[@]}" "${noble_packages[@]}"
		;;
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

case "${release}" in
	LinuxMint/*|Ubuntu/*|Debian/*)
		if ! "$script_dir/llvm.sh" 19; then
			echo "Error: Failed to install Clang 19 using llvm.sh script."
			exit 1
		fi

		;;
	*)
		set +x
		echo "Installation of clang19 SKIPPED, only in apt systems clang19 is installed automatically"
		set -x
esac

