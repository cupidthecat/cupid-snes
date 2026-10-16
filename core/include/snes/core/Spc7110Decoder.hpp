#pragma once
#include <array>
#include <cstdint>
#include <functional>

namespace snes::core {
class Spc7110Decoder {
public:
  using Reader = std::function<uint8_t(uint32_t)>;
  explicit Spc7110Decoder(Reader reader);
  void Initialize(unsigned mode, uint32_t origin);
  uint32_t NextRow();
  unsigned BitDepth() const noexcept {
    return depth_;
  }

private:
  struct Context {
    uint8_t model = 0;
    bool inverted = false;
  };
  Reader reader_;
  std::array<Context, 75> contexts_{};
  std::array<uint8_t, 16> colors_{};
  uint32_t address_ = 0;
  uint16_t interval_ = 256, code_ = 0;
  uint64_t history_ = 0;
  uint8_t decisions_ = 0, remaining_ = 8, depth_ = 1;
  bool Symbol(unsigned context);
  static void Prioritize(std::array<uint8_t, 16> &colors, uint8_t value);
};
} // namespace snes::core
