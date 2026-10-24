#include "cartridge/Spc7110TimingTrace.hpp"
#include "snes/core/Spc7110.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace snes::core;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class Operation> void Reject(Operation operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, "Malformed SPC7110 state must reject");
}
template <class Device> std::vector<uint8_t> Capture(Device &device) {
  StateArchive archive;
  device.SerializeState(archive);
  return archive.Bytes();
}
template <class Device> void Restore(Device &device, std::span<const uint8_t> bytes) {
  StateArchive archive(bytes);
  device.SerializeState(archive);
  archive.Commit();
}
template <class Device> void AtomicRejection(Device &device, std::vector<uint8_t> bytes) {
  const auto before = Capture(device);
  Reject([&] { Restore(device, bytes); });
  Check(Capture(device) == before,
        "Rejected SPC7110 state retains current operations and owner memory");
}

const std::vector<uint8_t> &Image(bool extended) {
  static const auto images = [] {
    std::array<std::vector<uint8_t>, 2> result;
    for (unsigned image = 0; image < 2; ++image) {
      auto &rom = result[image];
      rom.resize(image ? 0x700000 : 0x300000);
      for (size_t index = 0; index < rom.size(); ++index)
        rom[index] = uint8_t(index * 37 + (index >> 4) + 0xa5);
      for (unsigned entry = 0; entry < 16; ++entry) {
        const auto table = 0x100100 + entry * 4, origin = 0x1000 + entry * 0x1000;
        rom[table] = uint8_t(entry & 3);
        for (unsigned byte = 0; byte < 3; ++byte)
          rom[table + 1 + byte] = uint8_t(origin >> ((2 - byte) * 8));
      }
    }
    return result;
  }();
  return images[extended ? 1 : 0];
}

struct Board {
  const std::vector<uint8_t> &rom;
  std::vector<uint8_t> ram;
  unsigned hostCalls = 0;
  uint64_t elapsed = 0;
  Spc7110 chip;
  explicit Board(unsigned profile = 0, size_t size = 8192)
      : rom(Image((profile & 4) != 0)), ram(size, 0xff), chip(rom, ram, (profile & 2) != 0, [&] {
          ++hostCalls;
          return int64_t{1700000000};
        }) {
    chip.SetPal((profile & 1) != 0);
    Write(0x4801, 0);
    Write(0x4802, 1);
    Write(0x4803, 0);
    Write(0x4834, 1);
    Write(0x4830, 0x80);
    Write(0x006123, 0x39);
  }
  void Write(uint32_t address, uint8_t value) {
    chip.Write(address, value);
  }
  void Word(uint32_t address, uint32_t value, unsigned count = 2) {
    for (unsigned byte = 0; byte < count; ++byte)
      Write(address + byte, uint8_t(value >> (byte * 8)));
  }
  void Advance(uint32_t clocks) {
    elapsed += clocks;
    chip.Advance(clocks);
  }
  void Prefix(unsigned mode, unsigned mask, uint32_t clocks) {
    Write(0x4804, uint8_t(mode));
    Write(0x4805, 3);
    Write(0x4807, 2);
    Write(0x480b, 3);
    Word(0x4820, 0x91e3a7c5, 4);
    Word(0x4824, 0x8371);
    Word(0x4826, 0x0039);
    // Let those preparatory math writes finish before queuing the selected work.
    Advance(100);
    Write(0x482e, 1);
    if (mask & 1)
      Write(0x4806, 0);
    if (mask & 2)
      Write(0x4825, 0x93);
    if (mask & 4)
      Write(0x4827, 0);
    Advance(clocks);
    Write(0x4811, 0x93);
    Write(0x4812, 0x71);
    Write(0x4813, 0x31);
    Word(0x4814, 0xfff1);
    Word(0x4816, 0xfffd);
    Write(0x4818, 0x3f);
  }
  void SerializeState(StateArchive &archive) {
    archive.Fixed(std::span<uint8_t>(ram));
    archive.Number(elapsed);
    chip.SerializeState(archive);
  }
};

void TimingRegressions() {
  constexpr std::array<const char *, 8> expected{
      "6e1d364588c10add14976bf04e88a430ecd44e7e9b2ec493dba67543aac5a7ba",
      "b3b8f1379ea17384e229d43fec4048512fa72128242a30754f938f787ddc59d9",
      "38479b303792837e0c07dd2bbafa82feb2039fb63bbc121ee086e4a83ba4ec18",
      "aa2ba06a646bd9ac6fb24da22b297e56b7bab30a26e681da5c221274ced4974a",
      "7aea2bee7a2d60925fe5c7657d18f63cf05640f156586c49cba61723a94f554c",
      "36116be9f3272848fa8dbd1a5c2678e0bb7ef1f60cba35e9612024ee657ac5db",
      "cbb6ceb9ac0cd95f73bad3b67c5fb89f5eba15ad553deb62fcaaca267660a146",
      "d4c2897c518591a84cf00086e4d4971363f4883350a0e61e5e99078993b586b7"};
  for (unsigned profile = 0; profile < expected.size(); ++profile) {
    const auto bytes = spc7110_test::TimingTrace(profile);
    const auto digest = detail::Sha256(bytes);
    std::string hex;
    for (auto byte : digest) {
      hex += "0123456789abcdef"[byte >> 4];
      hex += "0123456789abcdef"[byte & 15];
    }
    Check(hex == expected[profile],
          "Mixed register, SRAM, RTC and DMA-port observations retain exact timing");
  }
}

void Continue(Board &first, Board &second, unsigned mode) {
  constexpr std::array<uint32_t, 16> reads{0x4800, 0x4809,   0x480a, 0x480c, 0x4810, 0x481a,
                                           0x4828, 0x4829,   0x482a, 0x482b, 0x482c, 0x482d,
                                           0x482f, 0x806123, 0x4841, 0x4842};
  for (unsigned step = 0; step < 110; ++step) {
    if (step == 0 || step == 7 || step == 35) {
      const std::array<std::pair<uint32_t, uint8_t>, 8> writes{
          {{0x4804, uint8_t((mode + step + 1) & 3)},
           {0x4805, uint8_t(step & 7)},
           {0x4820, uint8_t(step * 13 + 0x19)},
           {0x4821, 0x89},
           {0x4824, 0x71},
           {0x4826, uint8_t(step + 3)},
           {0x4818, uint8_t(step & 0x7f)},
           {0x006123, uint8_t(step * 7)}}};
      for (auto [address, value] : writes) {
        first.Write(address, value);
        second.Write(address, value);
      }
      if (step == 7) {
        first.Write(0x4806, 0);
        second.Write(0x4806, 0);
        first.Write(0x4825, 0x91);
        second.Write(0x4825, 0x91);
        first.Write(0x4827, 0);
        second.Write(0x4827, 0);
      }
    }
    for (auto address : reads)
      Check(first.chip.Read(address, 0x73) == second.chip.Read(address, 0x73),
            "Pending operations, partial tiles, data ports, arithmetic and RTC resume on the same "
            "edge");
    const auto clocks = step % 11 == 0 ? 0u : step % 3 == 0 ? 2u : 1u;
    first.Advance(clocks);
    second.Advance(clocks);
  }
  Check(Capture(first) == Capture(second),
        "SPC7110 continuation retains all internal and owner state");
}

void EveryWaitClock() {
  std::array<bool, 7> phases{};
  for (unsigned profile = 0; profile < 4; ++profile)
    for (unsigned mode = 0; mode < 4; ++mode)
      for (unsigned mask = 0; mask < 8; ++mask)
        for (uint32_t cut = 0; cut <= 95; ++cut) {
          Board first(profile), second(profile);
          first.Prefix(mode, mask, cut);
          second.Prefix((mode + 1) & 3, 7 - mask, 37);
          const auto before = first.elapsed;
          const auto bytes = Capture(first);
          // Five portable wait phases occur before the fixed 45-byte device tail.
          const auto phase = bytes[bytes.size() - 46];
          Check(phase < phases.size(), "Portable phase fits its encoded range");
          phases[phase] = true;
          auto *memory = second.ram.data();
          Restore(second, bytes);
          Check(first.elapsed == before && first.hostCalls == 0 && second.hostCalls == 0 &&
                    second.elapsed == first.elapsed && second.ram.data() == memory,
                "Mid-operation restore neither advances hardware nor samples host time or replaces "
                "SRAM");
          Continue(first, second, mode);
        }
  Check(phases[1] && phases[3] && phases[5] && phases[6] && !phases[2] && !phases[4],
        "Decode, multiply, divide and idle waits serialize without transient operation edges");
  Board cold, target;
  const auto bytes = Capture(cold);
  Check(bytes[bytes.size() - 46] == 0, "A newly powered device retains its initial loop position");
  Restore(target, bytes);
  Continue(cold, target, 0);
}

void ClockAndTileContinuation() {
  for (unsigned profile = 0; profile < 8; ++profile)
    for (unsigned mode = 0; mode < 3; ++mode)
      for (unsigned cut : {0u, 1u, 7u, 8u, 15u, 16u, 23u, 31u, 32u, 33u}) {
        Board first(profile), second(profile);
        first.Prefix(mode, 1, 21);
        for (unsigned byte = 0; byte < cut; ++byte)
          first.chip.Read(0x4800);
        first.Write(0x4840, 1);
        first.Write(0x4841, 3);
        first.Advance(71);
        Restore(second, Capture(first));
        Continue(first, second, mode);
        const auto sourceSave = first.chip.SaveRtc(), targetSave = second.chip.SaveRtc();
        Check(sourceSave == targetSave && first.hostCalls == ((profile & 2) ? 1u : 0u) &&
                  second.hostCalls == first.hostCalls,
              "Both target clock callbacks remain attached after tile and oscillator restoration");
      }
  Board first, second;
  first.Prefix(2, 7, 17);
  const auto bytes = Capture(first);
  first.chip.Reset();
  second.chip.Reset();
  Restore(second, bytes);
  Restore(first, bytes);
  Continue(first, second, 2);
}

void AtomicValidation() {
  Board source(2), target(2);
  source.Prefix(2, 7, 17);
  target.Prefix(1, 2, 3);
  const auto bytes = Capture(source);
  for (size_t prefix = 0; prefix < bytes.size(); prefix += 137)
    AtomicRejection(target, {bytes.begin(), bytes.begin() + prefix});
  AtomicRejection(target, {bytes.begin(), bytes.end() - 1});
  auto extra = bytes;
  extra.push_back(0);
  AtomicRejection(target, extra);
  const auto phase = bytes.size() - 46;
  for (auto value : {uint8_t{2}, uint8_t{4}, uint8_t{7}}) {
    auto bad = bytes;
    bad[phase] = value;
    AtomicRejection(target, bad);
  }
  for (auto [offset, value] : {std::pair{phase - 1, uint8_t{0xff}},
                               {phase + 1, uint8_t{2}},
                               {phase + 4, uint8_t{4}},
                               {phase + 5, uint8_t{32}},
                               {phase + 9, uint8_t{0x80}}}) {
    auto bad = bytes;
    bad[offset] = value;
    AtomicRejection(target, bad);
  }
  Board differentRegion(3), noClock(0), extended(6), noRam(2, 0);
  AtomicRejection(differentRegion, bytes);
  AtomicRejection(noClock.chip, Capture(source.chip));
  AtomicRejection(extended.chip, Capture(source.chip));
  AtomicRejection(noRam.chip, Capture(source.chip));
  Restore(target, bytes);
  Continue(source, target, 2);
}

std::vector<uint8_t> PortableFixture() {
  StateArchive archive;
  for (unsigned profile = 0; profile < 8; ++profile) {
    Board first(profile);
    first.Prefix(profile & 3, 7, 37);
    first.SerializeState(archive);
    for (unsigned step = 0; step < 41; ++step) {
      first.chip.Read(0x4800);
      first.chip.Read(0x4810);
      first.Advance(1);
    }
    first.SerializeState(archive);
  }
  return archive.Bytes();
}
} // namespace

int main() {
  try {
    TimingRegressions();
    EveryWaitClock();
    ClockAndTileContinuation();
    AtomicValidation();
    Check(!PortableFixture().empty(), "SPC7110 fixture retains all board variants");
    std::puts(
        "SPC7110 portable waits, timing, arithmetic, stream and RTC checkpoint checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
