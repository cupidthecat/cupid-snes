#pragma once

#include "snes/core/Msu1.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace snes::tests::msu_state {
inline void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
inline void Hash(uint64_t &hash, uint64_t value) {
  for (unsigned n = 0; n < 8; ++n)
    hash = (hash ^ uint8_t(value >> (n * 8))) * 1099511628211ull;
}
class Media {
public:
  explicit Media(const std::filesystem::path &name = "game.sfc", size_t size = 20013) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      auto candidate = std::filesystem::current_path() /
                       ("msu1-state-" + std::to_string(stamp) + "-" + std::to_string(attempt));
      if (std::filesystem::create_directory(candidate)) {
        directory_ = std::move(candidate);
        break;
      }
    }
    Check(!directory_.empty(), "Create unique MSU state media directory");
    rom = directory_ / name;
    std::vector<uint8_t> data(size);
    for (size_t n = 0; n < data.size(); ++n)
      data[n] = uint8_t(n * 13 + n / 127 + 0x39);
    Write(Data(), data);
    for (uint16_t track : {0, 1, 2, 3, 4, 5, 6, 65535}) {
      const size_t count = track == 3 ? 0 : track >= 4 && track <= 6 ? 1 : 5013;
      auto bytes = Pcm(track, count, track == 2 ? 0xffffffffu : track == 65535 ? 17 : 3);
      if (track == 4)
        bytes.resize(9);
      if (track == 5)
        bytes.resize(11);
      if (track == 6)
        bytes[0] = 'X';
      Write(Track(track), bytes);
    }
  }
  ~Media() {
    std::error_code error;
    for (const auto &path : files_)
      std::filesystem::remove(path, error);
    std::filesystem::remove(directory_, error);
  }
  Media(const Media &) = delete;
  Media &operator=(const Media &) = delete;
  std::filesystem::path Data() const {
    auto path = rom;
    path.replace_extension(".msu");
    return path;
  }
  std::filesystem::path Track(uint16_t track) const {
    auto path = rom;
    path.replace_extension();
    path += "-" + std::to_string(track) + ".pcm";
    return path;
  }
  static std::vector<uint8_t> Pcm(uint16_t track, size_t count, uint32_t loop = 3) {
    std::vector<uint8_t> bytes(8 + count * 4);
    std::copy_n("MSU1", 4, bytes.begin());
    for (unsigned n = 0; n < 4; ++n)
      bytes[4 + n] = uint8_t(loop >> (8 * n));
    for (size_t n = 0; n < count; ++n)
      for (unsigned side = 0; side < 2; ++side) {
        const auto value = uint16_t(n * 173 + track * 1201 + side * 3123);
        bytes[8 + n * 4 + side * 2] = uint8_t(value);
        bytes[9 + n * 4 + side * 2] = uint8_t(value >> 8);
      }
    return bytes;
  }
  void Write(const std::filesystem::path &path, std::span<const uint8_t> bytes) {
    if (std::find(files_.begin(), files_.end(), path) == files_.end())
      files_.push_back(path);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty())
      file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    file.close();
    Check(bool(file), "Write generated MSU state media");
  }
  std::filesystem::path rom;

private:
  std::filesystem::path directory_;
  std::vector<std::filesystem::path> files_;
};
inline void Select(snes::core::Msu1 &chip, uint16_t track, uint8_t control = 3) {
  chip.Write(0x2004, uint8_t(track));
  chip.Write(0x2005, uint8_t(track >> 8));
  chip.Write(0x2006, 255);
  chip.Write(0x2007, control);
}
inline void Seek(snes::core::Msu1 &chip, uint32_t offset) {
  for (unsigned n = 0; n < 4; ++n)
    chip.Write(0x2000 + n, uint8_t(offset >> (8 * n)));
}
struct Fixture {
  Media &media;
  snes::core::Msu1 chip;
  uint64_t samples = 0, audio = 14695981039346656037ull, ports = 0;
  explicit Fixture(Media &files, unsigned profile = 0) : media(files) {
    Check(chip.Open(files.rom), "Open generated state fixture");
    Select(chip, uint16_t(profile % 8), uint8_t(profile & 7));
  }
  void Step(unsigned step) {
    if (step % 97 == 3) {
      constexpr uint16_t tracks[]{0, 1, 2, 3, 4, 5, 6, 7, 65535};
      Select(chip, tracks[(step / 97) % 9], uint8_t(step & 7));
    }
    if (step % 31 == 7)
      chip.Write(0x2004, uint8_t(step));
    if (step % 37 == 11)
      chip.Write(0x2005, uint8_t(step >> 8));
    if (step % 17 == 3)
      chip.Write(0x2007, uint8_t(step & 7));
    if (step % 13 == 5)
      chip.Write(0x2006, uint8_t(step * 17));
    if (step % 19 == 2)
      Seek(chip, step * 31);
    if (step % 23 == 7)
      chip.Write(0x2001, uint8_t(step));
    if (step % 257 == 99)
      chip.Reset();
    for (unsigned n = 0; n < step % 7 + 1; ++n)
      Hash(ports, chip.Read(0x2001));
    Hash(ports, chip.Read(0x2000));
    const auto sample = chip.Clock(step % 11 == 7);
    ++samples;
    Hash(audio, std::bit_cast<uint32_t>(sample[0]));
    Hash(audio, std::bit_cast<uint32_t>(sample[1]));
  }
  void SerializeState(snes::core::detail::StateArchive &archive) {
    chip.SerializeState(archive);
    archive.Number(samples);
    archive.Number(audio);
    archive.Number(ports);
  }
};
} // namespace snes::tests::msu_state
