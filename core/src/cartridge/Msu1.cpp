// Streaming MSU-1 register and PCM implementation.

#include "snes/core/Msu1.hpp"
#include "snes/core/FilePath.hpp"

#include <algorithm>
#include <system_error>

namespace snes::core {
namespace {

uint32_t Little32(const uint8_t *bytes) {
  return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) | (uint32_t{bytes[2]} << 16) |
         (uint32_t{bytes[3]} << 24);
}

int16_t SignedLittle16(const uint8_t *bytes) {
  const int32_t value = int32_t{bytes[0]} | (int32_t{bytes[1]} << 8);
  return static_cast<int16_t>(value < 0x8000 ? value : value - 0x10000);
}

} // namespace

bool Msu1::Stream::Open(const std::filesystem::path &path) {
  Close();
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error))
    return false;
  file_.open(path, std::ios::binary | std::ios::ate);
  const auto end = file_.tellg();
  if (!file_ || end < std::streampos{0}) {
    Close();
    return false;
  }
  size_ = static_cast<uint64_t>(end);
  return true;
}

void Msu1::Stream::Close() {
  if (file_.is_open())
    file_.close();
  file_.clear();
  size_ = 0;
  cacheOffset_ = 0;
  cacheSize_ = 0;
}

bool Msu1::Stream::Read(uint64_t offset, std::span<uint8_t> destination) {
  if (!IsOpen() || offset > size_ || destination.size() > size_ - offset ||
      destination.size() > cache_.size())
    return false;
  if (destination.empty())
    return true;

  const bool cached = offset >= cacheOffset_ && offset - cacheOffset_ <= cacheSize_ &&
                      destination.size() <= cacheSize_ - static_cast<size_t>(offset - cacheOffset_);
  if (!cached) {
    cacheSize_ = 0;
    cacheOffset_ = offset;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file_)
      return false;
    const auto count =
        static_cast<std::streamsize>(std::min<uint64_t>(cache_.size(), size_ - offset));
    file_.read(reinterpret_cast<char *>(cache_.data()), count);
    cacheSize_ = static_cast<size_t>(file_.gcount());
    if (cacheSize_ < destination.size())
      return false;
  }
  const auto begin = static_cast<size_t>(offset - cacheOffset_);
  std::copy_n(cache_.data() + begin, destination.size(), destination.data());
  return true;
}

bool Msu1::Open(const std::filesystem::path &romPath, std::string *error) {
  Close();
  if (error)
    error->clear();
  if (romPath.empty()) {
    if (error)
      *error = "MSU-1 ROM path is empty";
    return false;
  }
  basePath_ = romPath;
  basePath_.replace_extension();
  auto dataPath = basePath_;
  dataPath += ".msu";
  if (!data_.Open(dataPath)) {
    if (error)
      *error = "Cannot open MSU-1 data file: " + PathText(dataPath);
    basePath_.clear();
    return false;
  }
  Reset();
  return true;
}

void Msu1::Close() {
  data_.Close();
  Reset();
  basePath_.clear();
}

void Msu1::Reset() {
  pcm_.Close();
  dataSeek_ = 0;
  dataOffset_ = 0;
  track_ = 0;
  status_ = 0;
  volume_ = 0;
  audioPosition_ = 0;
  playOffset_ = 0;
  loopOffset_ = 0;
  resume_ = {};
  if (IsOpen())
    OpenAudio();
}

uint8_t Msu1::Read(uint32_t address, uint8_t openBus) {
  if (!IsOpen() || !Selects(address))
    return openBus;
  const auto port = address & 7;
  if (port == 0)
    return status_ | 0x02;
  if (port == 1) {
    std::array<uint8_t, 1> value{};
    if (!data_.Read(dataOffset_, value))
      return 0;
    ++dataOffset_;
    return value[0];
  }
  static constexpr std::array<uint8_t, 6> signature{'S', '-', 'M', 'S', 'U', '1'};
  return signature[port - 2];
}

void Msu1::Write(uint32_t address, uint8_t value) {
  if (!IsOpen() || !Selects(address))
    return;
  const auto port = address & 7;
  if (port <= 3) {
    const auto shift = port * 8;
    dataSeek_ = (dataSeek_ & ~(uint32_t{0xff} << shift)) | (uint32_t{value} << shift);
    if (port == 3)
      dataOffset_ = dataSeek_;
    return;
  }
  if (port == 4) {
    track_ = static_cast<uint16_t>((track_ & 0xff00) | value);
  } else if (port == 5) {
    track_ = static_cast<uint16_t>((track_ & 0x00ff) | (uint16_t{value} << 8));
    SelectTrack();
  } else if (port == 6) {
    volume_ = value;
  } else if ((status_ & AudioError) == 0) {
    status_ = static_cast<uint8_t>((status_ & ~(Playing | Repeating)) | ((value & 3) << 4));
    if ((value & 5) == 4)
      resume_ = {true, track_, playOffset_};
  }
}

void Msu1::SelectTrack() {
  status_ &= static_cast<uint8_t>(~(Playing | Repeating));
  playOffset_ = 8;
  if (resume_.valid && resume_.track == track_) {
    playOffset_ = resume_.offset;
    resume_ = {};
  }
  OpenAudio();
}

void Msu1::OpenAudio() {
  pcm_.Close();
  status_ |= AudioError;
  auto path = basePath_;
  path += "-" + std::to_string(track_) + ".pcm";
  if (!pcm_.Open(path))
    return;
  std::array<uint8_t, 8> header{};
  if (!pcm_.Read(0, header) || header[0] != 'M' || header[1] != 'S' || header[2] != 'U' ||
      header[3] != '1') {
    pcm_.Close();
    return;
  }
  loopOffset_ = 8u + Little32(header.data() + 4) * 4u;
  if (loopOffset_ > pcm_.Size())
    loopOffset_ = 8;
  audioPosition_ = playOffset_;
  status_ &= static_cast<uint8_t>(~AudioError);
}

Msu1::OutputFrame Msu1::Clock(bool dspMuted) {
  if (!IsOpen() || !(status_ & Playing))
    return {};
  if (!pcm_.IsOpen()) {
    status_ &= static_cast<uint8_t>(~Playing);
    return {};
  }
  if (audioPosition_ >= pcm_.Size()) {
    if (status_ & Repeating) {
      playOffset_ = loopOffset_;
    } else {
      playOffset_ = 8;
      status_ &= static_cast<uint8_t>(~Playing);
    }
    audioPosition_ = playOffset_;
    return {}; // Seeking consumes a silent input tick.
  }
  std::array<uint8_t, 4> bytes{};
  const auto available =
      static_cast<size_t>(std::min<uint64_t>(bytes.size(), pcm_.Size() - audioPosition_));
  pcm_.Read(audioPosition_, std::span(bytes).first(available));
  audioPosition_ += available;
  playOffset_ += 4;
  if (dspMuted)
    return {};
  OutputFrame result{};
  for (unsigned side = 0; side < 2; ++side)
    result[side] =
        float(double(SignedLittle16(bytes.data() + side * 2)) / 32768.0 * volume_ / 255.0);
  return result;
}

} // namespace snes::core
