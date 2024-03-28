#!/usr/bin/env bash
set -eux -o pipefail
readonly script_dir="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")")"
# allow alias expansion in non-interactive shell
shopt -s expand_aliases
alias apt-get='apt-get --yes --option Dpkg::Options::="--force-confnew"'

die() {  echo "Error: ${*}" >&2;  exit 1; }

[ ${UID} -eq 0 ] || die "Run this script as root"

# Install necessary packages
apt-get install fuse libfuse3-dev
# necessary for debian
apt-get install sudo valgrind git rsyslog psmisc fio
apt-get install time bc
apt-get install vim kmod

# Install necessary packages for Ganesha tests
apt-get install \
	acl \
	asciidoc \
	bison \
	build-essential \
	byacc \
	ceph \
	cmake \
	dbus \
	docbook \
	docbook-xml \
	doxygen \
	fio \
	flex \
	krb5-user \
	libacl1-dev \
	libblkid-dev \
	libboost-filesystem-dev \
	libboost-iostreams-dev \
	libboost-program-options-dev \
	libboost-system-dev \
	libcap-dev \
	libcephfs-dev \
	libdbus-1-dev \
	libglusterfs-dev \
	libgssapi-krb5-2 \
	libjemalloc-dev \
	libjudy-dev \
	libkrb5-dev \
	libkrb5support0 \
	libnfsidmap-dev \
	libradospp-dev \
	libradosstriper-dev \
	librgw-dev \
	libsqlite3-dev \
	liburcu-dev \
	xfslibs-dev \
	tree

# Run SaunaFS setup script
mkdir -p /mnt/hdd_0 /mnt/hdd_1 /mnt/hdd_2 /mnt/hdd_3 /mnt/hdd_4 /mnt/hdd_5 /mnt/hdd_6 /mnt/hdd_7
readonly setup_machine_script="${script_dir}/../setup_machine.sh"
[ -f "${setup_machine_script}" ] || die "Script not found: ${setup_machine_script}"
grep -v '^[[:space:]]*mount[[:space:]]*[^[:space:]]*$' "${setup_machine_script}" \
	| bash -x /dev/stdin setup /mnt/hdd_0 /mnt/hdd_1 /mnt/hdd_2 /mnt/hdd_3 /mnt/hdd_4 /mnt/hdd_5 /mnt/hdd_6 /mnt/hdd_7
chown saunafstest:saunafstest /mnt/hdd_0 /mnt/hdd_1 /mnt/hdd_2 /mnt/hdd_3 /mnt/hdd_4 /mnt/hdd_5 /mnt/hdd_6 /mnt/hdd_7

# Extras
apt-get install \
  libdb-dev \
  libjudy-dev \
  pylint

GTEST_ROOT="${GTEST_ROOT:-"/usr/local"}"
readonly gtest_temp_build_dir="$(mktemp -d)"
apt-get install cmake libgtest-dev
cmake -S /usr/src/googletest -B "${gtest_temp_build_dir}" -DCMAKE_INSTALL_PREFIX="${GTEST_ROOT}"
make -C "${gtest_temp_build_dir}" install
rm -rf "${gtest_temp_build_dir:?}"

cp "${script_dir}/60-ip_port_range.conf" /etc/sysctl.d/

# Install zonefs-tools
readonly zonefs_tools_script="${script_dir}/zonefs-tools-setup.sh"
[ -f "${zonefs_tools_script}" ] || die "Script not found: ${zonefs_tools_script}"
"${zonefs_tools_script}" install
