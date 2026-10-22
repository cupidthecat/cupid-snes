#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace sdd1_test {

enum class Transfer {
  Switch,
  Priority,
  Alternating,
  Fixed,
  Increment,
  Decrement,
  Zero,
  Boundary,
  HdmaShared,
  HdmaSame
};

struct StreamProgram {
  std::vector<uint8_t> image;
  unsigned observations;
};

class StreamGuest {
public:
  void Write(uint32_t address, uint8_t value) {
    code_.insert(code_.end(), {0xa9, value, 0x8f, uint8_t(address), uint8_t(address >> 8),
                               uint8_t(address >> 16)});
  }
  void Read(uint32_t address) {
    code_.insert(code_.end(),
                 {0xaf, uint8_t(address), uint8_t(address >> 8), uint8_t(address >> 16), 0x8f,
                  uint8_t(output_), uint8_t(output_ >> 8), 0x7f});
    ++output_;
  }
  void Setup(unsigned channel, uint32_t address, unsigned count, uint8_t control = 8) {
    const std::array<uint8_t, 7> values{control,
                                        0x80,
                                        uint8_t(address),
                                        uint8_t(address >> 8),
                                        uint8_t(address >> 16),
                                        uint8_t(count),
                                        uint8_t(count >> 8)};
    for (unsigned port = 0; port < values.size(); ++port)
      Write(0x4300 + channel * 16 + port, values[port]);
  }
  void Arm(uint8_t mask) {
    Write(0x4800, mask);
    Write(0x4801, mask);
  }
  void Beam() {
    code_.insert(code_.end(), {0xad, 0x37, 0x21});
    Read(0x213f);
    for (uint16_t address : {0x213c, 0x213c, 0x213d, 0x213d})
      Read(address);
  }
  void NextFrame() {
    code_.insert(code_.end(), {0xad, 0x12, 0x42, 0x10, 0xfb, 0xad, 0x12, 0x42, 0x30, 0xfb});
    code_.insert(code_.end(), 32, 0xea);
  }
  StreamProgram Finish(std::vector<uint8_t> image, bool pal) {
    Write(0x7f1fff, 0xa5);
    code_.push_back(0xdb);
    std::copy(code_.begin(), code_.end(), image.begin());
    std::fill(image.begin() + 0x7fb0, image.begin() + 0x8000, 0);
    std::fill_n(image.begin() + 0x7fc0, 21, ' ');
    std::copy_n("SDD1 STREAM GUEST", 17, image.begin() + 0x7fc0);
    image[0x7fd5] = 0x32;
    image[0x7fd6] = 0x45;
    image[0x7fd7] = 13;
    image[0x7fd8] = 3;
    image[0x7fd9] = pal ? 2 : 1;
    image[0x7fda] = 0x33;
    image[0x7fdc] = image[0x7fdd] = 0xff;
    image[0x7ffd] = 0x80;
    return {std::move(image), output_};
  }

private:
  std::vector<uint8_t> code_{0x78};
  unsigned output_ = 0;
};

inline StreamProgram Stream(Transfer transfer, unsigned mode, bool pal) {
  std::vector<uint8_t> image(0x600000);
  for (unsigned index = 0; index < image.size(); ++index) {
    const unsigned low = index & 0xffff;
    image[index] = uint8_t(low * 37 + (low >> 8) * 13 + 0x53) ^ uint8_t(index >> 16);
  }
  const unsigned offset = mode * 0x1000, channel = mode & 7;
  uint32_t first = 0xc00000 + offset;
  const uint32_t second = 0xd00000 + offset;
  image[0x100000 + offset] = uint8_t(mode << 4);
  image[0x200000 + offset] = uint8_t(((mode + 5) & 15) << 4);
  StreamGuest guest;
  for (auto [address, value] : std::initializer_list<std::pair<uint32_t, uint8_t>>{{0x2100, 0x80},
                                                                                   {0x4200, 0},
                                                                                   {0x420c, 0},
                                                                                   {0x4804, 1},
                                                                                   {0x4805, 2},
                                                                                   {0x4806, 3},
                                                                                   {0x4807, 4},
                                                                                   {0x2181, 0},
                                                                                   {0x2182, 0},
                                                                                   {0x2183, 0}})
    guest.Write(address, value);
  if (transfer == Transfer::Switch) {
    guest.Setup(channel, first, 129);
    guest.Arm(uint8_t(1u << channel));
    for (unsigned index = 0; index < 3; ++index)
      guest.Read(first);
    guest.Read(0x4801);
    guest.Write(0x4801, 0);
    guest.Read(first);
    guest.Write(0x4801, uint8_t(1u << channel));
    guest.Write(0x4800, 0);
    guest.Read(first);
    guest.Write(0x4800, uint8_t(1u << channel));
    guest.Write(0x4804, 2);
    for (unsigned index = 0; index < 126; ++index)
      guest.Read(first);
  } else if (transfer == Transfer::Priority) {
    guest.Setup(0, first, 2);
    guest.Setup(7, first, 4);
    guest.Arm(0x81);
    guest.Write(0x420b, 0x80);
    guest.Read(0x4801);
    guest.Read(first);
    guest.Read(first);
  } else if (transfer == Transfer::Alternating) {
    guest.Setup(0, first, 3);
    guest.Setup(7, second, 5);
    guest.Arm(0x81);
    for (uint32_t address : {first, second, first, second, first, second, second, second}) {
      guest.Read(address);
      guest.Read(0x4801);
    }
  } else if (transfer == Transfer::HdmaShared || transfer == Transfer::HdmaSame) {
    const bool shared = transfer == Transfer::HdmaShared;
    std::fill_n(image.begin() + 0x100000 + offset, 65536, 0);
    image[0x100000 + offset] = uint8_t(mode << 4);
    guest.Setup(0, first, shared ? 1 : 17, 0);
    if (shared)
      guest.Setup(7, first, 17);
    guest.Arm(shared ? 0x81 : 1);
    guest.Write(0x420c, 1);
    guest.NextFrame();
    guest.Read(0x4801);
    const unsigned target = shared ? 7 : 0;
    guest.Write(0x4300 + target * 16, 8);
    guest.Write(0x420b, uint8_t(1u << target));
    guest.Write(0x420c, 0);
  } else {
    const unsigned count = transfer == Transfer::Zero                                      ? 0
                           : transfer == Transfer::Fixed || transfer == Transfer::Boundary ? 257
                                                                                           : 17;
    if (transfer == Transfer::Boundary) {
      first = 0xcffffe;
      image[0x1ffffe] = uint8_t(mode << 4);
      guest.Write(0x4805, 4);
    }
    const uint8_t control = transfer == Transfer::Increment   ? 0
                            : transfer == Transfer::Decrement ? 16
                                                              : 8;
    guest.Setup(channel, first, count, control);
    guest.Arm(uint8_t(1u << channel));
    if (transfer == Transfer::Fixed)
      guest.Read(first);
    guest.Beam();
    guest.Write(0x420b, uint8_t(1u << channel));
    guest.Beam();
    guest.Read(0x4801);
    if (transfer == Transfer::Increment || transfer == Transfer::Decrement)
      for (unsigned index = 0; index < 16; ++index)
        guest.Read(first);
  }
  for (uint32_t address : {0x4800u, 0x4801u, 0x4302 + channel * 16, 0x4303 + channel * 16,
                           0x4304 + channel * 16, 0x4305 + channel * 16, 0x4306 + channel * 16})
    guest.Read(address);
  guest.Beam();
  return guest.Finish(std::move(image), pal);
}

} // namespace sdd1_test
