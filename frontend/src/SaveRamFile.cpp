// Atomic battery RAM and clock sidecar persistence.

#include "SaveRamFile.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

namespace snes::frontend {
std::vector<uint8_t> SaveRamFile::Load(size_t expectedSize) {
    if (expectedSize == 0 || !std::filesystem::exists(path_))
        return {};
    if (std::filesystem::file_size(path_) != expectedSize)
        throw std::runtime_error("Save RAM file has the wrong size: " + core::PathText(path_));
    return Read(expectedSize);
}

std::vector<uint8_t> SaveRamFile::LoadUpTo(size_t maximumSize) {
    if (maximumSize == 0 || !std::filesystem::exists(path_))
        return {};
    const auto size = std::filesystem::file_size(path_);
    if (size > maximumSize)
        throw std::runtime_error("Battery save file is too large: " + core::PathText(path_));
    return Read(static_cast<size_t>(size));
}

std::vector<uint8_t> SaveRamFile::Read(size_t size) {
    std::vector<uint8_t> data(size);
    std::ifstream file(path_, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot read save RAM: " + core::PathText(path_));
    saved_ = data;
    return data;
}

void SaveRamFile::Flush(std::span<const uint8_t> data) {
    if (data.empty() || std::equal(data.begin(), data.end(), saved_.begin(), saved_.end()))
        return;
    if (!path_.parent_path().empty())
        std::filesystem::create_directories(path_.parent_path());
    WriteFileAtomically(path_, data);
    saved_.assign(data.begin(), data.end());
}
} // namespace snes::frontend
