# INSTALL - SaunaFS

You can install SaunaFS from pre-built packages or from source. The
pre-built packages are currently available for the following platforms:

- Linux (x86_64)
  - [Ubuntu 24.04 (Noble)](#ubuntu)
  - [Ubuntu 22.04 (Jammy)](#ubuntu)

Please follow the instructions below to install SaunaFS on your
system.

## Installing from pre-built packages

### Ubuntu 24.04 (Noble)

1. Import the public key used to sign the packages:

   ```shell
   gpg --no-default-keyring \
       --keyring /usr/share/keyrings/saunafs-archive-keyring.gpg \
       --keyserver hkps://keyserver.ubuntu.com \
       --receive-keys 0xA80B96E2C79457D4
   ```

   It will create a new keyring file `/usr/share/keyrings/saunafs-archive-keyring.gpg` and import the
   public key used to sign the packages. Please, notice that at the time of writing, [the use of apt-key deprecated](https://opensource.com/article/22/9/deprecated-linux-apt-key)

   You can verify the keyring file by running the following command:

   ```shell
   gpg --no-default-keyring \
       --keyring /usr/share/keyrings/saunafs-archive-keyring.gpg \
       --list-keys
   ```

2. Add the SaunaFS repository to your system:

   The repository is available at
   <https://repo.saunafs.com/repository/saunafs-ubuntu-24.04/> for Ubuntu 24.04
   or <https://repo.saunafs.com/repository/saunafs-ubuntu-22.04/> for Ubuntu 22.04.

   For Ubuntu 24.04, use:

   ```shell
   cat | sudo tee /etc/apt/sources.list.d/saunafs.list <<EOF
   deb [arch=amd64 signed-by=/usr/share/keyrings/saunafs-archive-keyring.gpg] https://repo.saunafs.com/repository/saunafs-ubuntu-24.04/ noble main
   EOF
   ```

   For Ubuntu 22.04:

   ```shell
   cat | sudo tee /etc/apt/sources.list.d/saunafs.list <<EOF
   deb [arch=amd64 signed-by=/usr/share/keyrings/saunafs-archive-keyring.gpg] https://repo.saunafs.com/repository/saunafs-ubuntu-22.04/ jammy main
   EOF
   ```

3. Update the package list:

   ```shell
   sudo apt update
   ```

4. Install SaunaFS:

   ```shell
   sudo apt update
   sudo apt install \
       saunafs-adm \
       saunafs-chunkserver \
       saunafs-cgi \
       saunafs-master
   ```

## Installing from source

SaunaFS is written in C++ and uses CMake as its build system. There are some utility scripts that can be used to
automate the build process. Please, follow the instructions below to build SaunaFS from source.

### Dependencies

### Dependencies needed to build deb packages

```text
acl asciidoc attr bc build-essential ca-certificates-java ccache cmake curl debhelper devscripts fuse3 git libblkid-dev libboost-filesystem-dev libboost-iostreams-dev libboost-program-options-dev libboost-system-dev libcrcutil-dev libdb-dev libfmt-dev libfuse3-dev libgoogle-perftools-dev libgtest-dev libisal-dev libjudy-dev libpam0g-dev libspdlog-dev libsystemd-dev liburcu-dev libyaml-cpp-dev lsb-release netcat-openbsd nfs4-acl-tools pkg-config pylint python3 python3-pip rsync socat sudo tidy time uuid-dev valgrind wget zlib1g-dev
```

The following scripts can be used to install the dependencies on Ubuntu 24.04 (Noble):

(elevated privileges are required)

```shell
./tests/ci_build/setup-build-machine.sh
```

...or, if you want to also install the dependencies to run the tests:

(elevated privileges are required)

```shell
./tests/ci_build/setup-test-machine.sh
```

Notice: _The above script will also create test users and modify system configurations (`/etc/fstab`, `sudoers`) to allow the
tests to run. Please, check the script and make sure you are comfortable before running it._

In case you want to run again the scripts above, you can use the following command to remove the installed packages:

(elevated privileges are required)

```shell
./tests/revert_setup_machine.sh
```

As it is a destructive operation, it will ask for confirmation before removing the packages.

### Building

The following script can be used to build SaunaFS with appropriate flags to run the tests

```shell
./tests/ci_build/run-build.sh test
```

If you want to build SaunaFS without the tests, you can use the following command:

(might require elevated privileges depending on your filesystem permissions)

```shell
./tests/ci_build/run-build.sh release
```

If you don't want to use the scripts above, you can use the following commands to build SaunaFS:

```shell
mkdir build

cmake -B ./build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -G 'Unix Makefiles' \
    -DENABLE_DOCS=ON \
    -DENABLE_CLIENT_LIB=ON \
    -DENABLE_TESTS=ON \
    -DENABLE_WERROR=ON

nice make -C ./build -j$(nproc)
```

### Installing

The following command can be used to install SaunaFS:

```shell
sudo make install
```
