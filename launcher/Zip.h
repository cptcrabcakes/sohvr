#pragma once

// Just enough zip to read a ROM out of one: the file list, and an entry's start or the whole of it. So the launcher
// needs no library, and can tell what's in a zip by inflating a few dozen bytes of it.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Launcher {

struct ZipEntry {
    std::string name;
    uint32_t method = 0;     // 0 stored, 8 deflated
    uint64_t offset = 0;     // of its local header
    uint64_t packedSize = 0;
    uint64_t size = 0;
};

bool ListZip(const std::filesystem::path& zip, std::vector<ZipEntry>& entries);
// Reads the first count bytes of an entry, or fewer if it's shorter.
bool ReadZipEntry(const std::filesystem::path& zip, const ZipEntry& entry, uint64_t count, std::vector<uint8_t>& out);

} // namespace Launcher
