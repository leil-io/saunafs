#!/usr/bin/env bash
set -eux -o pipefail
readonly self="$(readlink -f "${BASH_SOURCE[0]}")"
readonly script_name="$(basename "${self}")"

usage() {
	cat >&2 <<-EOF
		Usage: ${script_name} confirm

		This script reverts the changes applied by the setup_machine.sh script.
		It may be useful for debugging the setup_machine.sh script on new operating systems.
		It should be used __only__ by advanced users, who know what this script does.
		You need root permissions to run this script.
	EOF
	exit 1
}

if [[ ${1} != confirm ]]; then
	usage >&2
fi
shift

for username in saunafstest_{0..9}; do
	if getent passwd ${username} > /dev/null; then
		userdel ${username}
	fi
done

if getent passwd saunafstest > /dev/null; then
	userdel saunafstest
fi

if getent group saunafstest > /dev/null; then
	groupdel saunafstest
fi

rm -f /etc/sudoers.d/saunafstest
rm -rf /var/lib/saunafstest
rm -f /etc/security/limits.d/10-saunafstests.conf
rm -f /etc/saunafs_tests.conf
rm -f /etc/sysctl.d/60-ip_port_range.conf

mountpoint /mnt/ramdisk && umount /mnt/ramdisk
rm -rf /mnt/ramdisk

sed -i -e "/### Reload pam limits on sudo - necessary for saunafs tests. ###/d" /etc/pam.d/sudo
sed -i -e "/session required pam_limits.so/d" /etc/pam.d/sudo

sed -i -e "/# Ramdisk used in SaunaFS tests/d" /etc/fstab
sed -i -e "\[ramdisk  /mnt/ramdisk  tmpfs  mode=1777,size=2g[d" /etc/fstab
sed -i -e "/# Loop devices used in SaunaFS tests/d" /etc/fstab

temp_file=$(mktemp -p /tmp revert_setup_machine-etc-fstab.XXXXXXXXXX)
cp /etc/fstab "${temp_file}"

while IFS=$'\n' read -r line ; do
	file_system=$(echo "${line}" | awk '{print $1}')
	file_system_directory=$(dirname "${file_system}")
	mount_point=$(echo "${line}" | awk '{print $2}')

	mountpoint "${mount_point}" && umount -d "${mount_point}"
	[ -d "${mount_point}" ] && rmdir --ignore-fail-on-non-empty --parents "${mount_point}"
	rm -f "${file_system}"
	[ -d "${file_system_directory}" ] && rmdir --ignore-fail-on-non-empty --parents "${file_system_directory}"
	sed -i -e "\@${line}@d" /etc/fstab
done < <(grep "saunafstest_loop" "${temp_file}")

rm "${temp_file}"
set +x
echo "Reverted successfully"
