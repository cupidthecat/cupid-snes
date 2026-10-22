#include "snes/core/gameboy/Oam.hpp"

#include <algorithm>

namespace snes::core::gameboy {
namespace {
constexpr std::array<uint64_t, 1> WriteResolution = {0xe8};
constexpr std::array<uint64_t, 1> ReadResolution = {0xea};
constexpr std::array<uint64_t, 1> PairResolution = {0xecc8};
constexpr std::array<uint64_t, 1> GroupResolution = {0xf8f0f0e0};
constexpr std::array<uint64_t, 1> GroupThreeResolution = {0xf8f0f8e0};
constexpr std::array<uint64_t, 1> GroupFourResolution = {0xf8f0f0f0};
constexpr std::array<std::array<uint64_t, 4>, 2> CrossResolution = {
    {{0xfcfc0000fffc0000ull, 0xffffc0c0ffffc0c0ull, 0xffff0000ffff0000ull, 0xffffc0c0ffffc0c0ull},
     {0xfcfc0000fcfc0000ull, 0xffff80c0ffff80c0ull, 0xfefe0000fefe0000ull, 0xffffc0c0ffffc0c0ull}}};

// Resolve each bit independently when multiple scanner cells share the bus.
template <typename T, size_t N, size_t W>
T Resolve(const std::array<T, N> &cells, const std::array<uint64_t, W> &truth) noexcept {
  T result = 0;
  for (unsigned bit = 0; bit < sizeof(T) * 8; ++bit) {
    unsigned inputs = 0;
    for (unsigned cell = 0; cell < N; ++cell)
      inputs |= ((cells[cell] >> bit) & 1u) << cell;
    result |= T(((truth[inputs / 64] >> (inputs & 63)) & 1) << bit);
  }
  return result;
}
} // namespace

void Oam::Reset(bool secondModel) noexcept {
  bytes_.fill(0);
  secondModel_ = secondModel;
}

uint8_t Oam::Read(uint8_t address) const noexcept {
  return address < bytes_.size() ? bytes_[address] : 0;
}

void Oam::Write(uint8_t address, uint8_t value) noexcept {
  if (address < bytes_.size())
    bytes_[address] = value;
}

uint16_t Oam::Word(unsigned address) const noexcept {
  return uint16_t(bytes_[address] | (unsigned(bytes_[address + 1]) << 8));
}

void Oam::StoreWord(unsigned address, uint16_t value) noexcept {
  bytes_[address] = uint8_t(value);
  bytes_[address + 1] = uint8_t(value >> 8);
}

Oam::Row Oam::LoadRow(unsigned address) const noexcept {
  Row row;
  std::copy_n(bytes_.begin() + address, row.size(), row.begin());
  return row;
}

void Oam::StoreRow(unsigned address, const Row &row) noexcept {
  std::copy(row.begin(), row.end(), bytes_.begin() + address);
}

void Oam::AddressBus(uint8_t row) noexcept {
  if (row < 8 || row >= bytes_.size())
    return;
  const auto value = Resolve(std::array{Word(row - 8), Word(row - 4), Word(row)}, WriteResolution);
  auto previous = LoadRow(row - 8);
  previous[0] = uint8_t(value);
  previous[1] = uint8_t(value >> 8);
  StoreRow(row, previous);
}

void Oam::BlockedRead(uint8_t row) noexcept {
  if (row < 8 || row >= bytes_.size())
    return;
  uint16_t value;
  const unsigned group = (row / 8) % 4;
  if (group == 2) {
    value = Resolve(std::array{Word(row - 16), Word(row - 8), Word(row - 4), Word(row)},
                    PairResolution);
  } else if (group == 0) {
    if (row == 64) {
      value = Resolve(
          std::array{Word(0), Word(32), Word(48), Word(50), Word(56), Word(58), Word(60), Word(64)},
          CrossResolution[secondModel_]);
    } else {
      const auto &rule = secondModel_ || row == 32 ? GroupResolution
                         : row == 96               ? GroupThreeResolution
                                                   : GroupFourResolution;
      value = Resolve(
          std::array{Word(row - 32), Word(row - 16), Word(row - 8), Word(row - 4), Word(row)},
          rule);
    }
  } else {
    value = Resolve(std::array{Word(row - 8), Word(row - 4), Word(row)}, ReadResolution);
  }
  auto previous = LoadRow(row - 8);
  previous[0] = uint8_t(value);
  previous[1] = uint8_t(value >> 8);
  StoreRow(row - 8, previous);
  StoreRow(row, previous);
  if (group == 0 || group == 2)
    StoreRow(row - 16, previous);
  if (group == 0)
    StoreRow(row - 32, previous);
  if (row == 128)
    StoreRow(0, previous);
}

uint8_t Oam::ReadCpu(uint8_t address, uint8_t row, uint8_t blocked) noexcept {
  if (blocked & 2) {
    BlockedRead(row);
    return 0xff;
  }
  if (!(blocked & 1))
    return Read(address);
  if (address >= bytes_.size())
    return 0xff;
  const unsigned selected = address & 0xf8;
  if (row == 0) {
    const auto value =
        Resolve(std::array{Word(selected), Word(address & 0xfe), Word(0)}, ReadResolution);
    StoreWord(selected, value);
    auto first = LoadRow(selected);
    first[0] = uint8_t(value);
    first[1] = uint8_t(value >> 8);
    StoreRow(0, first);
  } else if (row == 160) {
    const unsigned pair = address & 6;
    if (pair != 4) {
      const auto &rule =
          pair == 2 || (pair == 0 && !secondModel_) ? WriteResolution : ReadResolution;
      const unsigned source = pair == 2 ? address & 0xfe : selected;
      const auto value = Resolve(std::array{Word(152 + pair), Word(source), Word(156)}, rule);
      StoreWord(152 + pair, value);
    }
    StoreRow(selected, LoadRow(152));
  }
  return 0xff;
}

void Oam::WriteCpu(uint8_t address, uint8_t value, uint8_t row, bool blocked) noexcept {
  if (blocked) {
    AddressBus(row);
    return;
  }
  if (address >= bytes_.size()) {
    if (row == 0)
      bytes_[address & 7] = value;
    return;
  }
  const unsigned selected = address & 0xf8;
  if (row == 160) {
    auto target = LoadRow(selected);
    const auto last = LoadRow(152);
    for (unsigned i = 0; i < target.size(); ++i)
      target[i] = (i / 2 == (address & 7) / 2)
                      ? Resolve(std::array{target[i], last[4], last[i]}, WriteResolution)
                      : last[i];
    StoreRow(selected, target);
  }
  bytes_[address] = value;
  if (row == 0) {
    const auto resolved =
        Resolve(std::array{Word(selected), Word(address & 0xfe), Word(0)}, WriteResolution);
    auto first = LoadRow(selected);
    first[0] = uint8_t(resolved);
    first[1] = uint8_t(resolved >> 8);
    StoreRow(0, first);
  }
}

} // namespace snes::core::gameboy
