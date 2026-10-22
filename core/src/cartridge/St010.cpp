#include "snes/core/St010.hpp"
#include "St010Tables.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>

namespace snes::core {
namespace {
int16_t Signed(uint16_t value) {
  return std::bit_cast<int16_t>(value);
}
int16_t Wrap(int value) {
  return Signed(static_cast<uint16_t>(value));
}
uint16_t Word(std::span<const uint8_t> ram, unsigned offset) {
  return uint16_t(ram[offset] | (uint16_t(ram[offset + 1]) << 8));
}
int16_t SWord(std::span<const uint8_t> ram, unsigned offset) {
  return Signed(Word(ram, offset));
}
int32_t Dword(std::span<const uint8_t> ram, unsigned offset) {
  return std::bit_cast<int32_t>(uint32_t(Word(ram, offset)) |
                                (uint32_t(Word(ram, offset + 2)) << 16));
}
void Put(std::span<uint8_t> ram, unsigned offset, uint32_t value, unsigned bytes = 2) {
  for (unsigned i = 0; i < bytes; ++i)
    ram[offset + i] = uint8_t(value >> (i * 8));
}
int Sin(uint16_t angle) {
  return st010::Sine[angle >> 8];
}
int Cos(uint16_t angle) {
  return Sin(uint16_t(angle + 0x4000));
}

struct Heading {
  int16_t x, y;
  uint16_t quadrant, angle;
};
Heading Compass(int16_t x, int16_t y) {
  Heading result{x, y, 0, 0};
  if (x < 0 && y < 0)
    result = {Wrap(-x), Wrap(-y), 0x8000, 0};
  else if (x < 0)
    result = {y, Wrap(-x), 0xc000, 0};
  else if (y < 0)
    result = {Wrap(-y), x, 0x4000, 0};
  while (result.x > 31 || result.y > 31) {
    if (result.x > 1)
      result.x >>= 1;
    if (result.y > 1)
      result.y >>= 1;
  }
  const unsigned index = (unsigned(result.x) & 31) * 32 + (unsigned(result.y) & 31);
  const unsigned angle = result.y ? 0x80 + st010::Arc[index] : 0x80;
  if (!result.y)
    result.quadrant = uint16_t(result.quadrant + 0x4000);
  result.angle = uint16_t((angle << 8) ^ result.quadrant);
  return result;
}

void Navigate(std::span<uint8_t> ram) {
  const int32_t y = Dword(ram, 0xc4), x = Dword(ram, 0xc8);
  const int targetY = SWord(ram, 0xc0), targetX = SWord(ram, 0xc2);
  uint16_t direction = Word(ram, 0xcc);
  uint16_t target = Compass(Wrap(targetY - (y >> 16)), Wrap(targetX - (x >> 16))).angle;
  const bool shifted = std::abs(int(target) - int(direction)) > 0x8000;
  if (shifted) {
    target = uint16_t(target + 0x8000);
    direction = uint16_t(direction + 0x8000);
  }
  const unsigned difference = unsigned(std::abs(int(target) - int(direction)));
  const uint16_t previous = Word(ram, 0xd4);
  uint16_t speed;
  if (difference == 0x8000)
    speed = 0x100;
  else if (difference >= 0x1000)
    speed = uint16_t(previous - (difference >> 4));
  else
    speed = std::min(uint16_t(previous + Word(ram, 0xd6)), Word(ram, 0xd8));
  if (std::abs(int(previous) - int(speed)) > 0x8000)
    speed = previous < speed ? 0 : 0xff00;
  if ((target > direction && target - direction > 0x80) ||
      (target < direction && direction - target >= 0x80))
    direction = uint16_t(direction + (target < direction ? -640 : 640));
  if (shifted)
    direction = uint16_t(direction - 0x8000);

  const auto distance = [](int targetPosition, int32_t position) {
    return std::bit_cast<int32_t>((uint32_t(targetPosition) << 16) - uint32_t(position)) >> 16;
  };
  const int dx = distance(targetX, x), dy = distance(targetY, y);
  const bool vertical = Word(ram, 0xda) != 0;
  const bool arrived = vertical ? dy >= -8 && dy <= 6 && dx >= -128 && dx <= 126
                                : dx >= -8 && dx <= 6 && dy >= -128 && dy <= 126;
  if (arrived) {
    Put(ram, 0xc0, Word(ram, 0xde));
    Put(ram, 0xc2, Word(ram, 0xe0) & 0x7fff);
    Put(ram, 0xdc, Word(ram, 0xdc) | 8);
  }
  const int movement = (speed >> 8) * 2;
  Put(ram, 0xc4, (uint32_t(y) - uint32_t((Sin(direction) >> 5) * movement)) & 0x1fffffff, 4);
  Put(ram, 0xc8, (uint32_t(x) - uint32_t((Cos(direction) >> 5) * movement)) & 0x1fffffff, 4);
  Put(ram, 0xcc, direction);
  Put(ram, 0xd4, speed);
  Put(ram, 0xd2, 0xffff);
  Put(ram, 0xda, 0);
}
} // namespace

bool St010::Selects(uint32_t address) noexcept {
  return (address & 0x788000) == 0x680000;
}

uint8_t St010::Read(uint32_t address, std::span<const uint8_t> ram) const {
  return ram[address & 0xfff];
}

void St010::Write(uint32_t address, uint8_t value, std::span<uint8_t> ram) {
  const unsigned offset = address & 0xfff;
  ram[offset] = value;
  if (offset == 0x21 && (value & 0x80)) {
    Execute(ram);
    ram[offset] &= 0x7f;
  }
}

void St010::Execute(std::span<uint8_t> ram) {
  switch (ram[0x20]) {
  case 1: {
    const auto heading = Compass(SWord(ram, 0), SWord(ram, 2));
    Put(ram, 0, uint16_t(heading.x));
    Put(ram, 2, uint16_t(heading.y));
    Put(ram, 4, heading.quadrant);
    Put(ram, 0x10, heading.angle);
    break;
  }
  case 2: {
    const unsigned count =
        unsigned(std::clamp(int(SWord(ram, 0x24)), 0, int((ram.size() - 0x80) / 2)));
    const auto exchange = [&](unsigned a, unsigned b) {
      const auto value = Word(ram, a);
      Put(ram, a, Word(ram, b));
      Put(ram, b, value);
    };
    for (unsigned end = count; end > 1; --end) {
      bool changed = false;
      for (unsigned i = 1; i < end; ++i) {
        const unsigned a = 0x40 + (i - 1) * 2, b = a + 2;
        if (Word(ram, a) >= Word(ram, b))
          continue;
        exchange(a, b);
        exchange(a + 0x40, b + 0x40);
        changed = true;
      }
      if (!changed)
        break;
    }
    break;
  }
  case 3:
    Put(ram, 0x10, uint32_t(int64_t(SWord(ram, 0)) * SWord(ram, 4) * 2), 4);
    Put(ram, 0x14, uint32_t(int64_t(SWord(ram, 2)) * SWord(ram, 4) * 2), 4);
    break;
  case 4: {
    const int64_t x = SWord(ram, 0), y = SWord(ram, 2);
    Put(ram, 0x10, uint16_t(std::sqrt(double(x * x + y * y))));
    break;
  }
  case 5:
    Navigate(ram);
    break;
  case 6:
    Put(ram, 0x10, uint32_t(int64_t(SWord(ram, 0)) * SWord(ram, 2) * 2), 4);
    break;
  case 7: {
    const auto angle = Word(ram, 0);
    for (unsigned row = 0; row < 176; ++row) {
      const unsigned divisor = 44 + row * 5;
      const int scale = int((39424 + divisor / 2) / divisor);
      const int a = (scale * Cos(angle)) >> 15;
      const int b = (scale * Sin(angle)) >> 15;
      Put(ram, 0xf0 + row * 2, uint16_t(a));
      Put(ram, 0x510 + row * 2, uint16_t(a));
      Put(ram, 0x250 + row * 2, uint16_t(b));
      Put(ram, 0x3b0 + row * 2, uint16_t(b ? ~b : 0));
    }
    break;
  }
  case 8: {
    const int x = SWord(ram, 0), y = SWord(ram, 2);
    const auto angle = Word(ram, 4);
    Put(ram, 0x10, uint16_t(((y * Sin(angle)) >> 15) + ((x * Cos(angle)) >> 15)));
    Put(ram, 0x12, uint16_t(((y * Cos(angle)) >> 15) - ((x * Sin(angle)) >> 15)));
    break;
  }
  default:
    break;
  }
}
} // namespace snes::core
