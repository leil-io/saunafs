#!/usr/bin/env bash


git diff --exit-code > /dev/null 2>&1
unstaged=$?

git diff --cached --exit-code > /dev/null 2>&1
staged=$?

if [[ $unstaged != 0 ]] || [[ $staged != 0 ]]; then
	echo "WARNING: You have uncommited changes. They will NOT be included in the build."
	read -n 1 -s -r -p "Press any key to continue"
	echo
fi

set -eux

export SAUNAFS_OFFICIAL_BUILD=NO

# Directories used by this script

output_dir=$(pwd)
source_dir=$(dirname "$0")

# DO NOT SET THIS AS EMPTY. If empty, this will delete all files in the current
# directory you are in
working_dir_prefix="/tmp/saunafs_deb_working_directory"

rm -rf ${working_dir_prefix:?}* || true
working_dir="$(mktemp -d ${working_dir_prefix}.$(date '+%F_%H%M%S').XXXXXX)"

os_release="$(lsb_release -si)/$(lsb_release -sr)"

# Systemd is added by default, except for the following systems

case "$os_release" in
  Debian*/7*)  use_systemd=0 ;;
  Ubuntu*/12*) use_systemd=0 ;;
  Ubuntu*/14*) use_systemd=0 ;;
  *) use_systemd=1 ;;
esac

# Create an empty working directory and clone sources there to make
# sure there are no additional files included in the source package

git clone "$source_dir" "$working_dir/saunafs"

cd "$working_dir/saunafs"

sed -i 's/make -C/make -j \$(nproc) -C/g' ./configure

# Move service files to debian/

cp -P rpm/service-files/* debian/

version="${VERSION_LONG_STRING:-"0.0.0-$(date -u +"%Y%m%d-%H%M%S")-devel"}"
export version

# Generate entry at the top of the changelog, needed to build the package
last_header=$(grep saunafs debian/changelog  | grep urgency | head -n 1)
status=$(echo "${version}" | cut -d'-' -f4)
package_name=$(echo "${last_header}" | awk '{print $1}')
changelog_version="${version%%-*}"
urgency=$(echo "${last_header}" | sed -e 's/^.*urgency=\(\w*\).*$/\1/')
(cat <<EOT
${package_name} (${changelog_version}) ${status}; urgency=${urgency}

  * Vendor ${status} release.
  * commit: $(git rev-parse HEAD)

 -- dev.saunafs.org package-builder <dev@saunafs.org>  $(date -R)

EOT
) | cat - debian/changelog > debian/changelog.tmp && mv debian/changelog.tmp debian/changelog

# Build packages.
dpkg_genchanges_params="-uc -us -F --changes-option=-Dversion=${version}"
if [[ $use_systemd == 0 ]]; then
	# shellcheck disable=SC2086
	dpkg-buildpackage ${dpkg_genchanges_params} -R'debian/rules-nosystemd'
else
	# shellcheck disable=SC2086
	dpkg-buildpackage ${dpkg_genchanges_params}
fi

# Copy all the created files and clean up

cp "$working_dir"/saunafs?* "$output_dir"
rm -rf "$working_dir"
