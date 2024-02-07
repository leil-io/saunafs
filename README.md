# [![SaunaFS](https://saunafs.com/favicon.svg) SaunaFS](https://saunafs.com/) [![Slack](https://img.shields.io/badge/slack-join_us-olive?logo=slack&style=flat)](https://saunafs.slack.com/)

Welcome to SaunaFS, a robust distributed POSIX file system meticulously designed to revolutionize your storage solutions
by offering unmatched efficiency, security, and redundancy. At its core, SaunaFS is a distributed file system primarily
written in C++, inspired by the pioneering concepts introduced by [Google File System](https://en.wikipedia.org/wiki/Google_File_System).

## Introduction

At SaunaFS, we are driven by a simple yet powerful mission: to revolutionise data storage by providing purpose-built solutions that are efficient, eco-friendly, and cost-effective. Our software-defined storage platform addresses the challenges of legacy data storage, offering a new way for companies to manage their data workloads effectively.

SaunaFS is a distributed file system that leverages the latest technologies and innovations to provide a high-performance, fault-tolerant, and cost-effective storage solution for data-intensive applications. SaunaFS is built on the principles of simplicity, reliability, and continuity, which are inspired by the Estonian sauna culture.

Simplicity means that SaunaFS is easy to install, configure, and use, without requiring complex tuning or management.

Reliability means that SaunaFS is resilient to failures and errors, and can recover quickly and gracefully.

Continuity means that SaunaFS installations can easily be refreshed with newer hardware without downtime and that the users will receive latest and greatest in technology with rolling updates once this is implemented into the product.

SaunaFS is also user-friendly and flexible, and supports various protocols like NFS and S3, provides proprietary WIndows Client, Mac OS client Linux native FUSE client.

### TL;DR

In a nutshell, SaunaFS is software-defined storage platform that was born out of the need to address the shortcomings of traditional data storage solutions.
Legacy storage often lacks optimization for specific workloads, resulting in inefficiencies, high energy consumption, and increased costs.
We set out to change this by creating a storage solution that leverages cutting-edge technologies, reduces environmental impact, and offers unmatched affordability.

## Feature List & Description

Resilient architecture to ensure seamless operation organized into distinct components (Metadata servers, data servers, clients)

Continuous assured data integrity and verification with CRC data stored within each chunk’s metadata.

Robust redundancy and enhanced data durability with Reed-Solomon erasure coding when up to two nodes can disappear without service interruption.

Instant Copy-on-Write Snapshots to implement immutability. SaunaFS provides copy-on-write snapshots for data integrity and immutability, with a historical record of files to ensure they remain unchanged.

Data preservation and recovery with instant snapshotting mechanism that provides an extra defense from accidental data deletions, corruptions and allowing for quick and easy recovery minimizing downtime and data loss.

Fast Metadata Logging for Advanced Security Analysis with support for access time attributes that allows for enhanced security insights, rapid search and analysis, and regulatory compliance.

Seamless Hardware Refresh and Expansion Without Downtime with SaunaFS capabilities providing for full hardware refresh and capacity addition without any disruptions to data access.

## Quick Start

Please refer to the [Installation Guide](INSTALL.md) for detailed instructions on how to install SaunaFS.

## Contact us

Join our Slack community at <https://saunafs.slack.com> to connect with fellow SaunaFS enthusiasts, developers, and users. In our Slack channels, you can:

- **Ask Questions**: Seek guidance, share your experiences, and ask questions related to SaunaFS.
- **Discuss Ideas**: Engage in discussions about new features, improvements, and best practices.
- **Receive Updates**: Stay informed about SaunaFS developments, releases, and events.

Slack is the hub for real-time conversations and knowledge-sharing within our community. Join us and be part of the discussion.

### Other ways to contact us
| Method                     | Link                                                          |
|----------------------------|---------------------------------------------------------------|
| :email: Email              | [contact@saunafs.com](mailto:contact@saunafs.com?subject=RFI) |
| :globe_with_meridians: Web | <https://saunafs.com>                                         |

Thank you for your help.

The SaunaFS Team.
