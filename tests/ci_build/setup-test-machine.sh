#!/usr/bin/env bash
set -eux -o pipefail
readonly script_dir="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")")"
# allow alias expansion in non-interactive shell
shopt -s expand_aliases
alias apt-get='apt-get --yes --option Dpkg::Options::="--force-confnew"'

die() {  echo "Error: ${*}" >&2;  exit 1; }

[ ${UID} -eq 0 ] || die "Run this script as root"

# Run SaunaFS setup script
mkdir -p /mnt/hdd_0 /mnt/hdd_1 /mnt/hdd_2 /mnt/hdd_3 /mnt/hdd_4 /mnt/hdd_5 /mnt/hdd_6 /mnt/hdd_7
readonly setup_machine_script="${script_dir}/../setup_machine.sh"
[ -f "${setup_machine_script}" ] || die "Script not found: ${setup_machine_script}"
grep -v '^[[:space:]]*mount[[:space:]]*[^[:space:]]*$' "${setup_machine_script}" \
	| bash -x /dev/stdin setup /mnt/hdd_0 /mnt/hdd_1 /mnt/hdd_2 /mnt/hdd_3 /mnt/hdd_4 /mnt/hdd_5 /mnt/hdd_6 /mnt/hdd_7
chown saunafstest:saunafstest /mnt/hdd_0 /mnt/hdd_1 /mnt/hdd_2 /mnt/hdd_3 /mnt/hdd_4 /mnt/hdd_5 /mnt/hdd_6 /mnt/hdd_7

# Extras
# Install zonefs-tools
readonly zonefs_tools_script="${script_dir}/zonefs-tools-setup.sh"
[ -f "${zonefs_tools_script}" ] || die "Script not found: ${zonefs_tools_script}"
"${zonefs_tools_script}" install
