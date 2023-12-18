#pragma once

#include <string>

/**
 * MemoryFile is a class that represents a file in memory.
 * It is used to store the contents of a file in memory.
 * It is a RAII wrapper around mmap
 */
class MemoryMappedFile {
public:
	/**
	 * Constructor
	 * @param path The path to the file
	 * @param size The size of the file
	 */
	explicit MemoryMappedFile(const std::string &path);

	/**
	 * Destructor
	 */
	virtual ~MemoryMappedFile();

	/**
	 * Read a block of data from the file
	 * @param offset The start of the block
	 * @return The block of data
	 */
	[[nodiscard]] uint8_t *seek(size_t offset) const;


	[[nodiscard]] size_t read(size_t &offset, uint8_t *buf, size_t size,
	                          bool walk = false) const;

	[[nodiscard]] size_t offset(const uint8_t *ptr) const;

	[[nodiscard]] const std::string &filename() const { return path; }

private:
	std::string path{};      // The path to the file
	size_t file_size{};      // The size of the file
	size_t map_size;         // The size of the memory mapping
	uint8_t *map = nullptr;  // The memory mapped contents of the file
	int fd;                  // The file descriptor of the file
};
