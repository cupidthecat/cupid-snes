#pragma once

#include "session/LaunchRequest.hpp"
#include "snes/core/Platform.hpp"
#include "snes/core/StateOperation.hpp"
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace snes::frontend {
struct StateFile {
  static constexpr size_t maximumMachineBytes = 128 * 1024 * 1024;
  std::vector<uint8_t> machine;
  core::VideoFrame preview;
  uint64_t created = 0;

  static StateFile Read(const std::filesystem::path &path,
                        const core::StateOperation &operation = {});
  static core::VideoFrame ReadPreview(const std::filesystem::path &path,
                                      const core::StateOperation &operation = {});
  static void Write(const std::filesystem::path &path, std::span<const uint8_t> machine,
                    const core::VideoFrame &picture, const core::StateOperation &operation = {});
};

class StateStorage {
public:
  static constexpr unsigned quickSlots = 10;
  struct Key {
    bool quick = false;
    std::string name;
    bool operator==(const Key &) const = default;
  };
  struct Entry {
    Key key;
    uintmax_t bytes = 0;
    std::filesystem::file_time_type modified;
  };

  StateStorage(const LaunchRequest &request, const std::filesystem::path &directory = {});
  static Key Quick(unsigned slot);
  static void ValidateName(const std::string &name);
  const std::filesystem::path &Directory() const noexcept {
    return directory_;
  }
  std::filesystem::path Path(const Key &key) const;
  std::vector<Entry> List(bool quick) const;
  void Rename(const Key &from, const std::string &name) const;
  void Remove(const Key &key) const;

private:
  std::filesystem::path directory_;
};
} // namespace snes::frontend
