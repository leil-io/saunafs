## Description of protocol

This is not a complete description, but describes the very basics. Contributions
are welcome.

There are two types of protocol used. One is older and used for legacy purposes,
and a newer version.

Both protocols have a header and include at least two fields: type and length.
All the types and what kind of data it includes are described in
SFSCommunication.h. The format in the comments are:

- name:\[bits\], where the bits part describes what integer type it is (for
example, chunkid:64 represents a 64-bit integer, while status:8 is a 8-bit integer)
- name:STDSTRING, where the data type is a string
- name:(N * \[various data\]), represents a vector for various data types

All multi-byte values are ordered big-endian.

### V1 protocol

The first version of the protocol uses a data type and length fields as its
header fields, both of which are 32-bit integers (may be both unsigned and
signed). The length field indicates the length of the data after the header.

Strings include a 8, 16 or 32-bit integer before the string to indicate the size
of the string. The string does not end with a NULL byte.

### V2 protocol

The header now includes a version after length, which is a 32-byte unsigned
integer. The length is both the version length and data length.

These are prefixed by "SAU" in SFSCommunication.h, and includes several changes
to the first protcocol:

- String lengths are always 32-bit unsigned integers.
- Strings end with a NULL byte.
- Vectors now include a 32-bit integer 

In addition, the packet types can have multiple versions, which is indicated
by `version==N`. The default version is 0.
