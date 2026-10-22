#pragma once

#include "../st010_cases.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace st010_test {
struct Scene {
  unsigned command, index;
};
inline constexpr std::array<Scene, 16> Scenes{{{1, 0},
                                               {1, 37},
                                               {2, 5},
                                               {2, 31},
                                               {3, 72},
                                               {3, 1000},
                                               {4, 3},
                                               {4, 7},
                                               {5, 0},
                                               {5, 1537},
                                               {6, 8},
                                               {6, 104},
                                               {7, 1},
                                               {7, 123},
                                               {8, 50},
                                               {8, 1500}}};

class Guest {
public:
  explicit Guest(bool pal) : image(0x100000) {
    for (size_t i = 0; i < image.size(); ++i)
      image[i] = uint8_t(0x80 | (i >> 15));
    std::fill_n(image.begin() + 0x7fb0, 80, 0);
    image[0x7fbf] = 1;
    std::fill_n(image.begin() + 0x7fc0, 21, ' ');
    std::copy_n("F1 ROC II", 9, image.begin() + 0x7fc0);
    image[0x7fd5] = 0x30;
    image[0x7fd6] = 0xf6;
    image[0x7fd7] = 10;
    image[0x7fd9] = pal ? 2 : 0;
    image[0x7fda] = 0x33;
    image[0x7fdc] = image[0x7fdd] = 255;
    image[0x7ffd] = 0x80;
    code.push_back(0x78);
  }
  void Write(uint32_t address, uint8_t value) {
    code.insert(code.end(), {0xa9, value, 0x8f});
    Address(address);
  }
  void Read(uint32_t address, uint32_t output) {
    code.push_back(0xaf);
    Address(address);
    code.push_back(0x8f);
    Address(output);
  }
  void Copy(uint32_t from, uint32_t to) {
    code.insert(code.end(), {0xa2, 0, 0, 0xbf});
    Address(from);
    code.push_back(0x9f);
    Address(to);
    code.insert(code.end(), {0xe8, 0xe0, 0, 0x10, 0xd0, 0xf2});
  }
  std::vector<uint8_t> Finish() {
    Write(0x7e0120, 0xa5);
    code.push_back(0xdb);
    std::copy(code.begin(), code.end(), image.begin());
    return std::move(image);
  }
  std::vector<uint8_t> image, code;

private:
  void Address(uint32_t address) {
    for (unsigned byte = 0; byte < 3; ++byte)
      code.push_back(uint8_t(address >> (byte * 8)));
  }
};

inline std::vector<uint8_t> PortGuest(bool pal) {
  Guest guest(pal);
  unsigned offset = 0x200;
  const auto read = [&](uint32_t address) { guest.Read(address, 0x7e0000 + offset++); };
  read(0x680020);
  guest.Write(0x680020, 0x55);
  read(0x680020);
  guest.Write(0x680021, 0x12);
  read(0x680021);
  read(0x601234);
  read(0xe01234);
  guest.Write(0xe80123, 0x73);
  read(0x680123);
  read(0xef7123);
  guest.Write(0x680123, 0x93);
  read(0xe80123);
  read(0x700123);
  read(0xf00123);
  read(0x708123);
  read(0xf08123);
  guest.Write(0x680000, 12);
  guest.Write(0x680002, 13);
  guest.Write(0xef7020, 6);
  guest.Write(0xef7021, 0x85);
  read(0x680020);
  read(0x680021);
  read(0x680010);
  read(0x680011);
  guest.Write(0x680000, 7);
  guest.Write(0x680002, 9);
  guest.Write(0x680021, 0x87);
  read(0x680020);
  read(0x680021);
  read(0x680010);
  guest.Write(0x680020, 0x55);
  guest.Write(0x680021, 0x91);
  read(0x680020);
  read(0x680021);
  read(0x680010);
  return guest.Finish();
}

inline std::vector<uint8_t> CommandGuest(bool pal) {
  Guest guest(pal);
  guest.code.insert(guest.code.end(), {0x18, 0xfb, 0xc2, 0x10});
  for (unsigned i = 0; i < Scenes.size(); ++i) {
    auto input = Input(Scenes[i].command, Scenes[i].index);
    input[0x20] = input[0x21] = 0;
    const unsigned offset = 0x10000 + i * 0x1000;
    std::copy(input.begin(), input.end(), guest.image.begin() + offset);
    const uint32_t address = ((offset / 0x8000) << 16) | 0x8000 | (offset & 0x7fff);
    guest.Copy(address, 0x680000);
    guest.Write(0x680020, uint8_t(Scenes[i].command));
    guest.Write(0x680021, 0x85);
    guest.Copy(0x680000, 0x7f0000 + i * 0x1000);
  }
  return guest.Finish();
}
} // namespace st010_test
