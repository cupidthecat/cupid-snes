#pragma once

#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/gameboy/Adapter.hpp"
#include "snes/core/gameboy/Peripherals.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <vector>

namespace snes::tests::handheld {
inline void Hash(uint64_t &hash, uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte)
    hash = (hash ^ uint8_t(value >> (8 * byte))) * 1099511628211ull;
}
inline std::array<uint8_t, 256> Boot() {
  std::array<uint8_t, 256> bytes{};
  bytes[0] = 0xc3;
  bytes[1] = 0xfc;
  bytes[0xfc] = 0x3e;
  bytes[0xfd] = 1;
  bytes[0xfe] = 0xe0;
  bytes[0xff] = 0x50;
  return bytes;
}
inline std::vector<uint8_t> Image(uint8_t mapper = 3, unsigned variant = 0) {
  const size_t size = variant == 1 ? 0x80000 : variant == 2 ? 0x400000 : 0x20000;
  std::vector<uint8_t> image(size);
  for (size_t n = 0; n < size; ++n)
    image[n] = uint8_t((n / 0x4000) * 37 + n * 13 + 7);
  image[0x147] = mapper;
  image[0x149] = 3;
  if (mapper == 0xbc) {
    image[0x149] = 0xc1;
    image[0x14a] = 0x65;
    image[0x152] = 1;
    image[0x153] = 8;
  }
  if (variant == 1)
    std::copy_n(image.begin() + 0x104, 48, image.begin() + 0x40104);
  image[0x100] = 0xc3;
  image[0x101] = 0x50;
  image[0x102] = 1;
  std::vector<uint8_t> code;
  const auto put = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
  const auto io = [&](uint8_t port, uint8_t value) { put({0x3e, value, 0xe0, port}); };
  put({0x31, 0xfe, 0xff, 0x21, 0, 0x80, 0x06, 32});
  const auto tile = code.size();
  put({0x78, 0xee, 0x5a, 0x22, 0x05, 0x20, 0});
  code.back() = uint8_t(int(tile) - int(code.size()));
  put({0x21, 0, 0xfe});
  for (uint8_t value : {16, 8, 1, 0})
    put({0x3e, value, 0x22});
  put({0x3e, 10, 0xea, 0, 0});
  for (const auto &pair : std::array<std::array<uint8_t, 2>, 18>{{{0x26, 0x80},
                                                                  {0x24, 0x77},
                                                                  {0x25, 0xff},
                                                                  {0x11, 0x80},
                                                                  {0x12, 0xf3},
                                                                  {0x13, 0xf0},
                                                                  {0x14, 0x83},
                                                                  {0x21, 0xa2},
                                                                  {0x22, 0x36},
                                                                  {0x23, 0x80},
                                                                  {0x06, 0x39},
                                                                  {0x05, 0xfd},
                                                                  {0x07, 5},
                                                                  {0x01, 0x96},
                                                                  {0x02, 0x81},
                                                                  {0x47, 0xe4},
                                                                  {0x4a, 0},
                                                                  {0x4b, 7}}})
    io(pair[0], pair[1]);
  io(0x40, 0xf3);
  put({0x21, 0, 0xc0});
  const auto loop = code.size();
  put({0x34, 0x7e, 0xe0, 0x43, 0xe0, 0x00, 0xf0, 0x44, 0xee, 0x33, 0xea, 0x10, 0xa0, 0x18, 0});
  code.back() = uint8_t(int(loop) - int(code.size()));
  std::copy(code.begin(), code.end(), image.begin() + 0x150);
  return image;
}
struct Samples final : snes::core::gameboy::Audio::Output {
  uint64_t count = 0, hash = 14695981039346656037ull, callbacks = 0;
  void Sample(uint64_t clock, int16_t left, int16_t right) override {
    ++callbacks;
    ++count;
    Hash(hash, clock);
    Hash(hash, uint16_t(left));
    Hash(hash, uint16_t(right));
  }
  void SerializeState(snes::core::detail::StateArchive &archive) {
    archive.Number(count);
    archive.Number(hash);
  }
};
struct MachineStateFixture {
  unsigned model;
  uint64_t now = 1700000000, timeReads = 0, resets = 0;
  Samples samples;
  snes::core::gameboy::Host host{[this] {
    ++resets;
    machine.Reset(model != 0, true);
  }};
  snes::core::gameboy::Peripherals devices{host, samples};
  snes::core::gameboy::Machine machine{host, devices};
  std::vector<uint8_t> image = Image(0x10);
  std::array<uint8_t, 256> boot = Boot();
  explicit MachineStateFixture(unsigned secondModel) : model(secondModel) {
    auto startup = std::make_shared<snes::core::gameboy::StartupState>();
    startup->Configure(true, 0x739155ull);
    machine.SetStartupState(startup);
    machine.GameCartridge().SetTimeSource([this] {
      ++timeReads;
      return now;
    });
    if (!machine.Load(image, boot))
      throw std::runtime_error("Load generated handheld state fixture");
    host.Write(0x6003, 0x80);
    for (unsigned instruction = 0; instruction < 500; ++instruction)
      machine.RunInstruction();
  }
  void Edit(unsigned step) {
    now = 1700000000 + step * 61;
    machine.WriteMemory(0xff43, uint8_t(step));
    machine.WriteMemory(0xff4b, uint8_t(step * 17));
    machine.WriteMemory(0xff45, uint8_t(step));
    host.Write(0x6004 + (step & 3), uint8_t(step * 31));
    if (step % 7 == 3)
      machine.WriteMemory(0xff04, 0);
    if (step % 11 == 5)
      machine.WriteMemory(0xff46, uint8_t(0xc0 + (step & 1)));
  }
  void SerializeState(snes::core::detail::StateArchive &archive) {
    archive.Expect(model);
    host.SerializeState(archive);
    devices.SerializeState(archive);
    machine.SerializeState(archive);
    samples.SerializeState(archive);
  }
};
struct AdapterStateFixture {
  unsigned profile;
  uint64_t now = 1700000000, timeReads = 0, callbacks = 0;
  uint64_t rawCount = 0, rawHash = 14695981039346656037ull;
  uint64_t mixedCount = 0, mixedHash = 14695981039346656037ull;
  snes::core::gameboy::Adapter adapter;
  std::vector<uint8_t> image = Image(0x10);
  std::array<uint8_t, 256> boot = Boot();
  explicit AdapterStateFixture(unsigned settings) : profile(settings), adapter(settings & 1) {
    adapter.SetPal(settings & 2);
    adapter.SetTimeSource([this] {
      ++timeReads;
      return now;
    });
    adapter.SetAudioOutput([this](int16_t left, int16_t right, uint32_t rate) {
      ++callbacks;
      ++rawCount;
      Hash(rawHash, uint16_t(left));
      Hash(rawHash, uint16_t(right));
      Hash(rawHash, rate);
    });
    adapter.SetMixedAudioOutput([this](float left, float right) {
      ++callbacks;
      ++mixedCount;
      Hash(mixedHash, std::bit_cast<uint32_t>(left));
      Hash(mixedHash, std::bit_cast<uint32_t>(right));
    });
    auto startup = std::make_shared<snes::core::gameboy::StartupState>();
    startup->Configure(true, 0x739155ull);
    adapter.InitializeStartup(startup);
    if (!adapter.Load(image, boot))
      throw std::runtime_error("Load generated handheld adapter fixture");
    adapter.Write(0x6003, 0x80 | ((profile >> 2) & 3));
    if (profile & 4)
      adapter.SetExpansionAudioRate(32000);
    for (unsigned step = 0; step < 64; ++step)
      Feed(step, 257);
  }
  void Feed(unsigned step, unsigned clocks) {
    now = 1700000000 + step * 61;
    adapter.WriteConsoleAudio(int16_t(step * 13), int16_t(-int(step * 7)));
    if ((step & 7) == 4)
      adapter.WriteExpansionAudio(0.125f, -0.25f);
    adapter.Write(0x6004 + (step & 3), uint8_t(step * 31));
    adapter.Advance(clocks, true);
  }
  void SerializeState(snes::core::detail::StateArchive &archive) {
    archive.Expect(profile);
    adapter.SerializeState(archive);
    archive.Number(rawCount);
    archive.Number(rawHash);
    archive.Number(mixedCount);
    archive.Number(mixedHash);
  }
};
} // namespace snes::tests::handheld
