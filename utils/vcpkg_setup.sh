#!/usr/bin/env bash
set -xeu -o pipefail

: "${VCPKG_ROOT:="${HOME}/vcpkg"}"

is_empty_or_does_not_exist() {
	local directory="${1}"
	[ ! -d "${directory}" ] || [ -z "$(ls -A "${directory}")" ]
}

export_on_profile() {
	local profile="${1}"
	local variable="${2}"
	local value="${3}"
	if ! grep -q "export ${variable}=" "${profile}"; then
		echo "export ${variable}=${value}" >> "${profile}"
	else
		sed -i "s|export ${variable}=.*|export ${variable}=${value}|" "${profile}"
	fi
}

add_to_path() {
	local profile="${1}"
	local value="${2}"
	if ! grep -qw "PATH=.*${value}" "${profile}"; then
		echo "export PATH=\"\${PATH}:${value}\"" >> "${profile}"
	fi
}

# Install vcpkg
if is_empty_or_does_not_exist "${VCPKG_ROOT}"; then
	git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}"
else
	git -C "${VCPKG_ROOT}" pull
fi

(
	cd "${VCPKG_ROOT}"
	./bootstrap-vcpkg.sh -disableMetrics
)

# Add vcpkg to PATH
export_on_profile "${HOME}/.bashrc" "VCPKG_ROOT" "\"${VCPKG_ROOT}\""
add_to_path "${HOME}/.bashrc" "\${VCPKG_ROOT}"
export VCPKG_ROOT="${VCPKG_ROOT}"
export PATH="${PATH}:${VCPKG_ROOT}"
