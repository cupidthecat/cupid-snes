// Cartridge bus, oscillator, and lifecycle for the handheld adapter.
#pragma once

#include "snes/core/gameboy/AudioMixer.hpp"
#include "snes/core/gameboy/Peripherals.hpp"

namespace snes::core {
class MemoryBus;
}

namespace snes::core::gameboy {

class Adapter final : private Audio::Output {
public:
  using AudioSink = std::function<void(int16_t left, int16_t right, uint32_t sampleRate)>;

  explicit Adapter(bool secondModel);
  Adapter(const Adapter &) = delete;
  Adapter &operator=(const Adapter &) = delete;
  bool Load(std::span<const uint8_t> image, std::span<const uint8_t> boot,
            std::string *error = nullptr);
  void Reset();
  void SetPal(bool pal) noexcept;
  void Advance(uint32_t masterClocks);
  uint8_t Read(uint32_t address);
  void Write(uint32_t address, uint8_t value);
  static bool Selects(uint32_t address) noexcept;
  static void MapCpu(MemoryBus &bus, uint8_t slot);
  std::vector<uint8_t> SaveBattery() const;
  void LoadBattery(std::span<const uint8_t> data);
  void SetAudioOutput(AudioSink output) {
    audioOutput_ = std::move(output);
  }
  void SetMixedAudioOutput(AudioMixer::Output output) {
    mixer_.SetOutput(std::move(output));
  }
  void WriteConsoleAudio(int16_t left, int16_t right) {
    mixer_.WriteConsole(left, right);
  }
  void SetConsoleAudioRate(uint32_t rate) {
    mixer_.SetConsoleRate(rate);
  }
  void SetExpansionAudioRate(uint32_t rate) {
    mixer_.SetExpansionRate(rate);
  }
  void WriteExpansionAudio(float left, float right) {
    mixer_.WriteExpansion(left, right);
  }

private:
  bool secondModel_ = false, loaded_ = false;
  uint32_t masterHz_ = 21'477'272;
  int64_t clockCredit_ = 0;
  AudioSink audioOutput_;
  AudioMixer mixer_;
  Host host_{[this] { ResetHandheld(); }};
  Peripherals devices_{host_, *this};
  Machine machine_{host_, devices_};

  uint32_t ClockRate() const noexcept;
  void ResetHandheld();
  void Sample(uint64_t clock, int16_t left, int16_t right) override;
};

} // namespace snes::core::gameboy
