<p align="center">
    <img alt="SaunaFS" style="width: 50%; height: auto;" src="https://s3.diaway.com/files/saunafs/H_Logo_Colored_B_BG_WHITE.png"/>
</p>
<h3 align="center">A Distributed POSIX File System</h3>

[![Slack](https://img.shields.io/badge/slack-join_us-olive?logo=slack&style=flat)](https://saunafs.slack.com/)

## About

[SaunaFS](https://saunafs.com) is a free-and open source, distributed POSIX
file system inspired by [Google File
System](https://en.wikipedia.org/wiki/Google_File_System). Designed to run on
commodity hardware, SaunaFS is a high-performance, scalable, and reliable file
system that provides high availability, data integrity, fault tolerance, and
performance on par with local file systems. It it easy to deploy and manage,
and it is designed to be used in a wide range of applications, from small
clusters to large data centers.


### Feature List

* Resilient architecture to ensure seamless operation organized into distinct components (Metadata servers, data servers, clients).
* Continuous assured data integrity and verification with CRC data stored within each chunk’s metadata.
* Robust redundancy and enhanced data durability with Reed-Solomon erasure coding when up to two nodes can disappear without service interruption.
* Instant Copy-on-Write Snapshots to implement immutability.
* Data preservation and recovery with instant snapshotting mechanism.
* Fast metadata logging for  with support for access time attribute.
* Seamless hardware refresh and expansion without downtime.

## Quick Start

### Installation

Please refer to the [Installation Guide](INSTALL.md) for detailed instructions
on how to install SaunaFS.

### Setup

Check the [Quick Start guide](https://docs.saunafs.com/quick-start) for a
simple setup of SaunaFS on a single machine.

After the Quick Start Guide, for an advanced setup, please refer to the
[Administration Guide](https://docs.saunafs.com/advanced-setup) as a starting
place.

### Building from source

This section assumes you have the necessary dependencies installed. If not,
check the [Installation Guide](INSTALL.md) for a list of dependencies (at least
for Ubuntu) and a more complete guide for compiling from source.

We use `nice` to set the building process to a lower priority, so it doesn't
hog memory and CPU resources. We also set `-j` to the number of cores in your
system to speed up the build process. Note that setting `-j` without nice can
lead to the system running out of memory/hanging.

```bash
git clone https://github.com/leil-io/saunafs.github
cd saunafs
mkdir build
cd build
cmake ..
nice -n 16 make -j$(nproc)
```

## Documentation

There are 2 types of documentation available:

* [Online documentation for a general overview](https://docs.saunafs.com/)
* [Man pages for specific commands and service configuration](docs/)

## Contributing

***Currently, we do not accept outside contributions. We still allow (and
encourage) pull requests, but without approval they won't be merged at the
moment. We are currently working on creating a system to allow seamless pull
requests outside with as little fuss as possible. See this
[issue](https://github.com/leil-io/saunafs/issues/8) for more details.***

See the [Contributing Guide](CONTRIBUTING.md) for detailed information on how
to contribute to SaunaFS.

The [Developer Guide](https://docs.saunafs.com/dev-guide) is a good starting
point for how to setup a development environment and run tests.

## Contact us

Join our [Slack community](https://saunafs.slack.com) to connect with fellow
SaunaFS enthusiasts, developers, and users. In our Slack channels, you can:

- **Ask Questions**: Seek guidance, share your experiences, and ask questions
related to SaunaFS.
- **Discuss Ideas**: Engage in discussions about new features, improvements,
and best practices.
- **Receive Updates**: Stay informed about SaunaFS developments, releases, and
events.

Join us and be part of the discussion.

### Other ways to contact us
| Method                     | Link                                                          |
|----------------------------|---------------------------------------------------------------|
| :email: Email              | [contact@saunafs.com](mailto:contact@saunafs.com?subject=RFI) |
| :globe_with_meridians: Web | <https://saunafs.com>                                         |

Thank you for your help.

The SaunaFS Team.
