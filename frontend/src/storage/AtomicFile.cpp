#include "AtomicFile.hpp"
#include "snes/core/FilePath.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <stdexcept>

namespace snes::frontend {
std::vector<uint8_t> ReadFile(const std::filesystem::path& path, size_t maximumSize,
                              const core::StateOperation& operation) {
    const auto label = operation ? core::PathText(path.filename()) : std::string{};
    operation.Report(core::StatePhase::ReadFile, 0, 0, label);
    if (!std::filesystem::exists(path))
        return {};
    const auto size = std::filesystem::file_size(path);
    if (size > maximumSize)
        throw std::runtime_error("File is too large: " + core::PathText(path));
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read file: " + core::PathText(path));
    for (size_t offset = 0; offset < bytes.size(); offset += 65536) {
        operation.Report(core::StatePhase::ReadFile, offset, bytes.size(), label);
        const auto count = std::min<size_t>(65536, bytes.size() - offset);
        if (!file.read(reinterpret_cast<char*>(bytes.data() + offset), std::streamsize(count)))
            throw std::runtime_error("Cannot read file: " + core::PathText(path));
    }
    operation.Report(core::StatePhase::ReadFile, bytes.size(), bytes.size(), label);
    return bytes;
}

void WriteFileAtomically(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                         const core::StateOperation& operation) {
    WriteFileAtomically(
        path, bytes.size(),
        [&](uint64_t offset, std::span<uint8_t> part) {
            std::copy_n(bytes.data() + size_t(offset), part.size(), part.data());
        },
        operation);
}

void WriteFileAtomically(const std::filesystem::path& path, uint64_t size,
                         const std::function<void(uint64_t, std::span<uint8_t>)>& fill,
                         const core::StateOperation& operation) {
    if (!fill)
        throw std::invalid_argument("File producer is missing");
    const auto label = operation ? core::PathText(path.filename()) : std::string{};
    operation.Report(core::StatePhase::WriteFile, 0, size, label);
    static std::atomic<unsigned> serial{0};
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(SDL_GetTicksNS()) + "-" + std::to_string(serial++);

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{temporary};

    std::ofstream file(temporary, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot create file: " + core::PathText(temporary));
    std::array<uint8_t, 65536> buffer;
    for (uint64_t offset = 0; offset < size;) {
        operation.Report(core::StatePhase::WriteFile, offset, size, label);
        const auto count = size_t(std::min<uint64_t>(buffer.size(), size - offset));
        fill(offset, std::span(buffer).first(count));
        file.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(count));
        if (!file)
            throw std::runtime_error("Cannot write file: " + core::PathText(path));
        offset += count;
    }
    file.close();
    if (!file)
        throw std::runtime_error("Cannot write file: " + core::PathText(path));
    operation.Report(core::StatePhase::WriteFile, size, size, label);
    const auto source = core::PathText(temporary), destination = core::PathText(path);
    if (!SDL_RenamePath(source.c_str(), destination.c_str()))
        throw std::runtime_error("Cannot replace file: " + core::PathText(path) + ": " +
                                 SDL_GetError());
}
} // namespace snes::frontend
