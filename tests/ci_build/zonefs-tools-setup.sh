#!/usr/bin/env bash
set -eu -o pipefail
readonly script_name="$(basename "${BASH_SOURCE[0]}")"

readonly workspace="/tmp/mkzonefs-setup"
readonly zonefs_tools_version="${ZONEFS_TOOLS_VERSION:-v1.6.0}"

readonly action="${1:-"install"}"
case "${action:?}" in
    "install" | "uninstall")
        ;;
    *)
        echo "Usage: ${script_name} [ install | uninstall ]"
        exit 1
        ;;
esac

mkdir -p "${workspace:?}"
trap "/bin/rm -rf '${workspace:?}'" EXIT

( cd "${workspace:?}"
    git clone https://github.com/westerndigitalcorporation/zonefs-tools
    cd zonefs-tools
    git checkout "${zonefs_tools_version:?}"
    sh ./autogen.sh
    ./configure
    make
    sudo make "${action:?}"
)
