// Camera capture registers, sensor input, and tile conversion.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <vector>

namespace snes::core::gameboy {

class Camera {
public:
  using NoiseSource = std::function<uint8_t()>;
  Camera();
  void Reset() noexcept;
  uint8_t Read(uint16_t address) const noexcept;
  void Write(uint16_t address, uint8_t value);
  void Advance(unsigned clocks, bool sleeping = false) noexcept;
  bool Busy() const noexcept {
    return (registers_[0] & 1) != 0;
  }
  uint8_t ReadImage(uint16_t offset);
  bool SetFrame(std::span<const uint8_t> pixels);
  void SetEntropy(uint64_t seed, bool enabled) noexcept;
  void SetNoiseSource(NoiseSource source);

private:
  using Generator =
      std::linear_congruential_engine<uint64_t, 2862933555777941757ull, 3037000493ull, 0>;
  Generator random_;
  NoiseSource noiseSource_;
  std::array<uint8_t, 54> registers_{};
  std::vector<uint8_t> frame_;
  std::vector<uint16_t> colors_;
  uint32_t remaining_ = 0;
  uint8_t alignment_ = 0, noise_ = 0;
  bool randomEnabled_ = true, dirty_ = true;

  uint8_t Noise(unsigned x, unsigned y) const noexcept;
  void PrepareColors();
  int Color(unsigned x, unsigned y) const noexcept;
};

} // namespace snes::core::gameboy
