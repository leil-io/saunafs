#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <vector>

#include "memory_file.h"
#include "slogger.h"

class MemoryMapUtils {
public:
	/**
	 * Calculate the size of the memory mapping from the size of the file
	 * @param file_size The size of the file
	 * @return The size of the memory mapping
	 */
	inline static size_t calculateMapSizeFromFilesize(size_t file_size) {
		size_t map_size = file_size;
		size_t page_size = MemoryMapUtils::pageSize();
		size_t remainder = map_size % page_size;
		if (remainder != 0) {
			map_size += page_size - remainder;
		}
		return map_size;
	}

	inline static size_t pageSize() { return sysconf(_SC_PAGESIZE); }

	inline static size_t getFileSizeFromFd(int fd) {
		struct stat st {};
		if (fstat(fd, &st) == -1) {
			std::string errorMsg = "Failed to fstat file: " + std::string(strerror(errno));
			throw std::runtime_error(errorMsg);
		}
		return st.st_size;
	}
};

MemoryMappedFile::MemoryMappedFile(const std::string &path) {
	this->path = path;
	this->fd = ::open(path.c_str(), O_RDONLY);
	if (this->fd == -1) {
		throw std::runtime_error("Failed to open file '" + path +
		                         "' for reading");
	}
	this->file_size = MemoryMapUtils::getFileSizeFromFd(this->fd);
	this->map_size =
	    MemoryMapUtils::calculateMapSizeFromFilesize(this->file_size);

	this->map =
	    (uint8_t *)::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
	if (this->map == MAP_FAILED) {
		throw std::runtime_error("Failed to mmap file '" + path + "'");
	}
}

MemoryMappedFile::~MemoryMappedFile() {
	try {
		::munmap(this->map, this->map_size);
	} catch (...) {
		// Ignore, so the fd is closed
	}
	::close(this->fd);
}


uint8_t *MemoryMappedFile::seek(size_t offset) const {
	if (this->map == nullptr) {
		throw std::runtime_error("File is not mapped");
	}
	if (offset >= this->file_size) {
		throw std::runtime_error("Offset is out of bounds");
	}
	return this->map + offset;
}

size_t MemoryMappedFile::read(size_t &offset, uint8_t *buf, size_t size,
                              bool walk) const {
	if (this->map == nullptr) {
		throw std::runtime_error("File is not mapped");
	}
	auto ptr = this->map + offset;
	auto read_size = std::min(size, this->file_size - offset);
	memcpy(buf, ptr, read_size);
	if (walk) {
		offset += read_size;
	}
	return read_size;
}

size_t MemoryMappedFile::offset(const uint8_t *ptr) const {
	if (map == nullptr) {
		throw std::runtime_error("File is not mapped");
	}
	if (ptr == nullptr) {
		throw std::runtime_error("Pointer is null");
	}
	if (ptr < map || ptr >= map + file_size) {
		throw std::runtime_error("Pointer is out of bounds");
	}
	return ptr - map;
}
