#include "Resource.hpp"
#include "snes/core/FilePath.hpp"
#include "snes/core/detail/Sha256.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace snes::core {
namespace {
using Stamp = std::array<uint64_t, 8>;
bool Seek(std::FILE *file, int64_t offset, int origin) {
#ifdef _WIN32
  return ::_fseeki64(file, offset, origin) == 0;
#else
  static_assert(std::numeric_limits<off_t>::digits >= 63);
  return ::fseeko(file, off_t(offset), origin) == 0;
#endif
}
int64_t Position(std::FILE *file) {
#ifdef _WIN32
  return ::_ftelli64(file);
#else
  return int64_t(::ftello(file));
#endif
}
#ifdef _WIN32
struct ReadLease {
  HANDLE handle = INVALID_HANDLE_VALUE;
  explicit ReadLease(const std::filesystem::path &path) {
    if (path.empty())
      return;
    handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION)
      throw std::runtime_error("MSU-1 checkpoint media has an active writer");
  }
  ~ReadLease() {
    if (handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
  ReadLease(const ReadLease &) = delete;
  ReadLease &operator=(const ReadLease &) = delete;
};
std::optional<Stamp> FileStamp(HANDLE handle) {
  if (handle == INVALID_HANDLE_VALUE)
    return {};
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_BASIC_INFO basic{};
  FILE_ID_INFO identity{};
  const bool valid =
      GetFileInformationByHandle(handle, &info) &&
      GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) &&
      GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) &&
      GetFileType(handle) == FILE_TYPE_DISK && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  // Without a change time, equal-size edits can retain the entire cached stamp.
  if (!valid || basic.ChangeTime.QuadPart <= 0)
    return {};
  uint64_t low = 0, high = 0;
  std::memcpy(&low, identity.FileId.Identifier, sizeof(low));
  std::memcpy(&high, identity.FileId.Identifier + sizeof(low), sizeof(high));
  return Stamp{identity.VolumeSerialNumber,
               low,
               (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
               uint64_t(basic.LastWriteTime.QuadPart),
               uint64_t(basic.ChangeTime.QuadPart),
               0,
               uint64_t(basic.CreationTime.QuadPart),
               high};
}
std::optional<Stamp> Observe(std::FILE *file, const ReadLease &lease) {
  const auto held = FileStamp(reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(file))));
  auto current = FileStamp(lease.handle);
  if (!held || !current || (*held)[0] != (*current)[0] || (*held)[1] != (*current)[1] ||
      (*held)[7] != (*current)[7])
    return {};
  alignas(USN_RECORD) std::array<uint8_t, 4096> bytes{};
  DWORD count = 0;
  if (!DeviceIoControl(lease.handle, FSCTL_READ_FILE_USN_DATA, nullptr, 0, bytes.data(),
                       DWORD(bytes.size()), &count, nullptr) ||
      count < sizeof(USN_RECORD))
    return {};
  USN_RECORD record{};
  std::memcpy(&record, bytes.data(), sizeof(record));
  if (record.MajorVersion != 2 || record.RecordLength > count || record.Usn <= 0)
    return {};
  // File times can remain identical across multiple writes in one clock tick.
  (*current)[5] = uint64_t(record.Usn);
  return current;
}
#else
std::optional<Stamp> Observe(std::FILE *file) {
  struct stat info{};
  if (::fstat(::fileno(file), &info) || !S_ISREG(info.st_mode) || info.st_size < 0)
    return {};
#ifdef __APPLE__
  const auto modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
  const auto modified = info.st_mtim, changed = info.st_ctim;
#endif
  return Stamp{uint64_t(info.st_dev),      uint64_t(info.st_ino),
               uint64_t(info.st_size),     uint64_t(modified.tv_sec),
               uint64_t(modified.tv_nsec), uint64_t(changed.tv_sec),
               uint64_t(changed.tv_nsec),  0};
}
#endif
bool Attached([[maybe_unused]] const std::filesystem::path &path,
              [[maybe_unused]] const std::optional<Stamp> &held) {
#ifdef _WIN32
  return true;
#else
  struct stat binding{};
  return held && !path.empty() && !::stat(path.c_str(), &binding) &&
         uint64_t(binding.st_dev) == (*held)[0] && uint64_t(binding.st_ino) == (*held)[1];
#endif
}
} // namespace
Msu1::Stream::Resource::~Resource() {
  if (file)
    std::fclose(file);
}
std::shared_ptr<Msu1::Stream::Resource>
Msu1::Stream::Resource::Open(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error))
    return {};
  auto result = std::make_shared<Resource>();
#ifdef _WIN32
  result->file = ::_wfopen(path.c_str(), L"rb");
#else
  result->file = std::fopen(path.c_str(), "rb");
#endif
  // Stream owns every cached byte that can survive a checkpoint.
  if (!result->file || std::setvbuf(result->file, nullptr, _IONBF, 0) != 0 ||
      !Seek(result->file, 0, SEEK_END))
    return {};
  const auto end = Position(result->file);
  if (end < 0)
    return {};
  result->size = uint64_t(end);
  result->path = std::filesystem::absolute(path, error);
  if (error)
    result->path.clear();
  return result;
}
size_t Msu1::Stream::Resource::Read(uint64_t offset, std::span<uint8_t> bytes) {
  std::clearerr(file);
  if (offset > uint64_t(std::numeric_limits<int64_t>::max()) ||
      !Seek(file, int64_t(offset), SEEK_SET))
    return 0;
  return std::fread(bytes.data(), 1, bytes.size(), file);
}
const Msu1::Stream::Resource::Key &
Msu1::Stream::Resource::Fingerprint(const StateOperation &operation) {
  const auto label = operation ? PathText(path.filename()) : std::string{};
  operation.Report(StatePhase::Media, 0, size, label);
#ifdef _WIN32
  ReadLease lease(path);
  const auto observe = [&] { return Observe(file, lease); };
#else
  const auto observe = [&] { return Observe(file); };
#endif
  const auto before = observe();
  if (identity && stamp && before == stamp && Attached(path, before)) {
    operation.Report(StatePhase::Media, identity->size, identity->size, label);
    if (operation && (observe() != before || !Attached(path, before)))
      throw std::runtime_error("MSU-1 media changed while preparing a checkpoint");
    return *identity;
  }
  std::clearerr(file);
  const auto position = Position(file);
  struct Cursor {
    std::FILE *file;
    int64_t position;
    ~Cursor() {
      if (position >= 0)
        Seek(file, position, SEEK_SET);
      std::clearerr(file);
    }
  } cursor{file, position};
  if (!Seek(file, 0, SEEK_END))
    throw std::runtime_error("Cannot prepare MSU-1 media for a checkpoint");
  const auto end = Position(file);
  if (end < 0 || !Seek(file, 0, SEEK_SET))
    throw std::runtime_error("Cannot prepare MSU-1 media for a checkpoint");
  Key next;
  next.size = uint64_t(end);
  detail::Sha256Hasher hash;
  std::array<uint8_t, 65536> buffer;
  auto remaining = next.size;
  while (remaining) {
    operation.Report(StatePhase::Media, next.size - remaining, next.size, label);
    const auto count = size_t(std::min<uint64_t>(buffer.size(), remaining));
    if (std::fread(buffer.data(), 1, count, file) != count)
      throw std::runtime_error("MSU-1 media changed while preparing a checkpoint");
    hash.Update(std::span(buffer).first(count));
    remaining -= count;
  }
  operation.Report(StatePhase::Media, next.size, next.size, label);
  const auto after = observe();
  if (!Seek(file, 0, SEEK_END) || Position(file) != end || before != after)
    throw std::runtime_error("MSU-1 media changed while preparing a checkpoint");
  next.digest = hash.Finish();
  identity = next;
  stamp.reset();
  // Detached files retain their handles; recheck their bytes on each capture.
  if (after && (*after)[2] == next.size && Attached(path, after))
    stamp = after;
  return *identity;
}
} // namespace snes::core
