#!/usr/bin/env bash
set -eux -o pipefail
script_dir="$(readlink -f "$(dirname "${BASH_SOURCE[0]}")")"
# allow alias expansion in non-interactive shell
shopt -s expand_aliases
alias apt-get='apt-get --yes --assume-yes --option Dpkg::Options::="--force-confnew"'

die() {  echo "Error: ${*}" >&2;  exit 1; }

[ ${UID} -eq 0 ] || die "Run this script as root"

extract_paragraphs() {
	local search="${1}"
	local file="${2}"
	awk -v RS='' '/'"${search}"'/' "${file}"
}

setup_machine_script="${script_dir}/../setup_machine.sh"
[ -f "${setup_machine_script}" ] || die "Script not found: ${setup_machine_script}"

extract_paragraphs 'echo Install necessary programs' "${setup_machine_script}" | \
	bash -x /dev/stdin
