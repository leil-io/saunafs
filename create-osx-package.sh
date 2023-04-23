#!/usr/bin/env bash
set -eux

# Directories used by this script
output_dir=$(pwd)
source_dir=$(dirname "$0")
working_dir=/tmp/saunafs_osx_working_directory
install_dir=${working_dir}/saunafs/
sauna_version=$(grep "set(PACKAGE_VERSION_"  CMakeLists.txt |grep -E "(MAJOR|MINOR|MICRO)" |awk '{print substr($2, 1, length($2)-1)}' |xargs |sed 's/\ /./g')

# Create an empty working directory and clone sources there to make
# sure there are no additional files included in the source package
rm -rf "$working_dir"
mkdir "$working_dir"
git clone "$source_dir" "$working_dir/saunafs"

# Build packages.
cd "$working_dir/saunafs"
if [[ ${BUILD_NUMBER:-} && ${OFFICIAL_RELEASE:-} == "false" ]] ; then
	# Jenkins has called us. Add build number to the package version
	# and add information about commit to changelog.
	version="${sauna_version}.${BUILD_NUMBER}"
else
	version=$sauna_version
fi

mkdir build-osx
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
	-DENABLE_TESTS=NO \
	-DENABLE_DOCS=YES

make
make DESTDIR=${working_dir}/saunafs/build-osx/ install

pkgbuild --root ${working_dir}/saunafs/build-osx/ --identifier com.saunafs --version $version --ownership recommended ../saunafs-${version}.pkg

# Copy all the created files and clean up
cp "$working_dir"/saunafs/saunafs?* "$output_dir"
rm -rf "$working_dir"
