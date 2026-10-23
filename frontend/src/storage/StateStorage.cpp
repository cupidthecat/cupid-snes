#include "StateStorage.hpp"
#include "AtomicFile.hpp"
#include "snes/core/FilePath.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateDigest.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace snes::frontend {
namespace {
constexpr size_t headerSize = 64, previewWidth = 256, previewHeight = 240;
constexpr size_t maximumFileBytes =
    headerSize + StateFile::maximumMachineBytes + previewWidth * previewHeight * sizeof(uint32_t);
constexpr std::array<uint8_t, 8> magic{'C', 'U', 'P', 'I', 'D', 'S', 'F', 0};
void Put(std::vector<uint8_t> &out, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i)
    out.push_back(uint8_t(value >> (8 * i)));
}
uint64_t Get(std::span<const uint8_t> bytes, size_t offset, unsigned count) {
  uint64_t value = 0;
  for (unsigned i = 0; i < count; ++i)
    value |= uint64_t(bytes[offset + i]) << (8 * i);
  return value;
}
auto Digest(std::span<const uint8_t> bytes, const core::StateOperation &operation,
            core::StatePhase phase) {
  return core::detail::StateDigest(bytes.subspan(headerSize), operation, phase, bytes.first(32));
}
std::string Identity(const LaunchRequest &request) {
  std::vector<uint8_t> bytes{uint8_t(request.kind), request.competitionDip};
  for (const auto &path : request.paths) {
    const auto normalized = path.empty() || path == "-"
                                ? std::string{}
                                : core::PathText(std::filesystem::weakly_canonical(
                                      std::filesystem::absolute(core::FilePath(path))));
    Put(bytes, normalized.size(), 8);
    bytes.insert(bytes.end(), normalized.begin(), normalized.end());
  }
  std::string text;
  for (const auto byte : core::detail::Sha256(bytes)) {
    constexpr char hex[] = "0123456789abcdef";
    text += hex[byte >> 4];
    text += hex[byte & 15];
  }
  return text;
}
} // namespace

StateFile StateFile::Read(const std::filesystem::path &path,
                          const core::StateOperation &operation) {
  const auto bytes = ReadFile(path, maximumFileBytes, operation);
  if (bytes.size() < headerSize || !std::equal(magic.begin(), magic.end(), bytes.begin()) ||
      Get(bytes, 8, 4) != 1)
    throw std::runtime_error("Unsupported or missing state file: " + core::PathText(path));
  const auto machineSize = Get(bytes, 12, 4);
  const auto width = Get(bytes, 24, 4), height = Get(bytes, 28, 4);
  if (machineSize < 48 || machineSize > maximumMachineBytes ||
      !((width == 0 && height == 0) || (width == previewWidth && height == previewHeight)) ||
      bytes.size() != headerSize + machineSize + width * height * sizeof(uint32_t))
    throw std::runtime_error("Invalid state file size or preview");
  const auto digest = Digest(bytes, operation, core::StatePhase::Validate);
  if (!std::equal(digest.begin(), digest.end(), bytes.begin() + 32))
    throw std::runtime_error("State file checksum does not match");
  StateFile result;
  result.created = Get(bytes, 16, 8);
  result.machine.assign(bytes.begin() + headerSize, bytes.begin() + headerSize + machineSize);
  result.preview.width = uint32_t(width);
  result.preview.height = uint32_t(height);
  result.preview.pixels.reserve(size_t(width * height));
  for (size_t offset = headerSize + size_t(machineSize); offset < bytes.size(); offset += 4)
    result.preview.pixels.push_back(uint32_t(Get(bytes, offset, 4)));
  return result;
}

void StateFile::Write(const std::filesystem::path &path, std::span<const uint8_t> machine,
                      const core::VideoFrame &picture, const core::StateOperation &operation) {
  operation.Report(core::StatePhase::Capture);
  if (machine.size() < 48 || machine.size() > maximumMachineBytes)
    throw std::runtime_error("Machine state exceeds the supported file size");
  const bool preview = picture.width && picture.height &&
                       uint64_t(picture.width) * picture.height == picture.pixels.size();
  std::vector<uint8_t> bytes(magic.begin(), magic.end());
  bytes.reserve(headerSize + machine.size() + (preview ? previewWidth * previewHeight * 4 : 0));
  Put(bytes, 1, 4);
  Put(bytes, machine.size(), 4);
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  Put(bytes, uint64_t(std::max<int64_t>(0, now)), 8);
  Put(bytes, preview ? previewWidth : 0, 4);
  Put(bytes, preview ? previewHeight : 0, 4);
  bytes.resize(headerSize);
  bytes.insert(bytes.end(), machine.begin(), machine.end());
  if (preview) {
    for (size_t y = 0; y < previewHeight; ++y)
      for (size_t x = 0; x < previewWidth; ++x)
        Put(bytes,
            picture.pixels[(y * picture.height / previewHeight) * picture.width +
                           x * picture.width / previewWidth],
            4);
  }
  const auto digest = Digest(bytes, operation, core::StatePhase::Capture);
  std::copy(digest.begin(), digest.end(), bytes.begin() + 32);
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  WriteFileAtomically(path, bytes, operation);
}

StateStorage::StateStorage(const LaunchRequest &request, const std::filesystem::path &directory) {
  unsigned primary = 0;
  if (request.kind == LaunchRequest::Kind::Handheld ||
      (request.kind == LaunchRequest::Kind::SufamiTurbo && !request.paths[1].empty() &&
       request.paths[1] != "-"))
    primary = 1;
  const auto game = core::FilePath(request.paths[primary]);
  std::string label;
  for (const unsigned char c : core::PathText(game.stem())) {
    if (label.size() == 24)
      break;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')
      label += char(c);
    else
      label += '_';
  }
  const auto base = std::filesystem::weakly_canonical(
      std::filesystem::absolute(directory.empty() ? game.parent_path() / "states" : directory));
  directory_ = base / (label + "-" + Identity(request));
}

StateStorage::Key StateStorage::Quick(unsigned slot) {
  if (slot >= quickSlots)
    throw std::invalid_argument("Quick state slot must be between 1 and 10");
  return {true, std::to_string(slot + 1)};
}
void StateStorage::ValidateName(const std::string &name) {
  if (name.empty() || name.size() > 120 || name.front() == ' ' || name.back() == ' ' ||
      name.back() == '.' || name == "." || name == ".." ||
      name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
      std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
    throw std::invalid_argument("State name contains unsupported characters");
  auto stem = name.substr(0, name.find('.'));
  for (auto &c : stem)
    if (c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
  if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
      (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' &&
       stem[3] <= '9'))
    throw std::invalid_argument("State name is reserved by the operating system");
}
std::filesystem::path StateStorage::Path(const Key &key) const {
  ValidateName(key.name);
  if (key.quick) {
    bool found = false;
    for (unsigned slot = 0; slot < quickSlots; ++slot)
      found |= key == Quick(slot);
    if (!found)
      throw std::invalid_argument("Unknown quick state slot");
  }
  return directory_ / (key.quick ? "Quick" : "Managed") / core::FilePath(key.name + ".cst");
}
std::vector<StateStorage::Entry> StateStorage::List(bool quick) const {
  std::vector<Entry> result;
  const auto path = directory_ / (quick ? "Quick" : "Managed");
  if (!std::filesystem::exists(path))
    return result;
  for (const auto &file : std::filesystem::directory_iterator(path)) {
    if (!file.is_regular_file() || file.path().extension() != ".cst")
      continue;
    Key key{quick, core::PathText(file.path().stem())};
    try {
      (void)Path(key);
    } catch (const std::invalid_argument &) {
      continue;
    }
    result.push_back({std::move(key), file.file_size(), file.last_write_time()});
  }
  std::sort(result.begin(), result.end(), [quick](const Entry &a, const Entry &b) {
    return quick ? std::stoi(a.key.name) < std::stoi(b.key.name) : a.key.name < b.key.name;
  });
  return result;
}
void StateStorage::Rename(const Key &from, const std::string &name) const {
  if (from.quick)
    throw std::invalid_argument("Quick states keep their slot names");
  const auto source = Path(from), target = Path({false, name});
  if (source == target)
    return;
  // Both native operations reject an existing destination without replacing it.
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
    throw std::system_error(int(GetLastError()), std::system_category(), "Cannot rename state");
#elif defined(__linux__)
  if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE))
    throw std::system_error(errno, std::generic_category(), "Cannot rename state");
#else
  std::filesystem::create_hard_link(source, target);
  std::error_code error;
  if (!std::filesystem::remove(source, error) || error) {
    std::error_code ignored;
    std::filesystem::remove(target, ignored);
    throw std::runtime_error("Cannot rename state: " + core::PathText(source));
  }
#endif
}
void StateStorage::Remove(const Key &key) const {
  if (!std::filesystem::remove(Path(key)))
    throw std::runtime_error("State file no longer exists");
}
} // namespace snes::frontend
