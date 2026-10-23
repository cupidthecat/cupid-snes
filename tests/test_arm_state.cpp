#include "cartridge/ArmProcessorTimingTrace.hpp"
#include "snes/core/Arm6.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace snes::core;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
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
template <class Device> void Reject(Device &device, std::vector<uint8_t> bytes) {
  const auto before = Capture(device);
  bool rejected = false;
  try {
    Restore(device, bytes);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, "Invalid ARM state must reject");
  Check(Capture(device) == before,
        "Rejected ARM state retains the worker, RAM and all partial work");
}
template <class Device> void Truncation(Device &device) {
  const auto bytes = Capture(device);
  for (size_t prefix = 0; prefix < bytes.size(); prefix += std::max(size_t{1}, bytes.size() / 23))
    Reject(device, {bytes.begin(), bytes.begin() + prefix});
  Reject(device, {bytes.begin(), bytes.end() - 1});
  auto extra = bytes;
  extra.push_back(0);
  Reject(device, extra);
}
struct Machine : Arm6 {
  std::array<uint32_t, 256> memory{};
  uint32_t reads = 0, writes = 0, idles = 0;
  Machine() {
    Reset();
  }
  uint32_t ReadArm(unsigned access, uint32_t address) override {
    ++reads;
    const auto value = memory[(address & 1023) / 4];
    return (access & Width) == Byte ? (value >> ((address & 3) * 8)) & 255 : value;
  }
  void WriteArm(unsigned, uint32_t address, uint32_t value) override {
    ++writes;
    memory[(address & 1023) / 4] = value;
  }
  void IdleArm() override {
    ++idles;
  }
  void SerializeState(StateArchive &archive) {
    Arm6::SerializeState(archive);
    archive.Array(memory);
    archive.Number(reads);
    archive.Number(writes);
    archive.Number(idles);
  }
};
void CorePipeline() {
  Machine source, target;
  source.memory[0] = 0xe3a00001;
  source.memory[1] = 0xe3a01007;
  source.memory[2] = 0xeafffffd;
  source.RunInstruction();
  source.memory[1] = 0xe3a01009;
  Restore(target, Capture(source));
  source.RunInstruction();
  target.RunInstruction();
  Check(target.GetState().common[1] == 7, "Restore retains an already prefetched instruction");
  for (unsigned step = 0; step < 10; ++step) {
    source.RunInstruction();
    target.RunInstruction();
    Check(Capture(source) == Capture(target),
          "Branch refill retains exact pipeline and access sequencing");
  }
  Check(target.GetState().common[1] == 9, "Branch refill fetches the changed memory word");
  Truncation(target);
  for (unsigned cut = 0; cut < 6; ++cut) {
    Machine first, second;
    first.memory = {0xe321f0d2, 0xe3a0d00b, 0xef000000, 0xe3a02037, 0xeafffffe};
    for (unsigned step = 0; step < cut; ++step)
      first.RunInstruction();
    Restore(second, Capture(first));
    for (unsigned step = 0; step < 12; ++step) {
      first.RunInstruction();
      second.RunInstruction();
      Check(Capture(first) == Capture(second),
            "Exception modes retain all banked and saved registers");
    }
  }
}
std::vector<uint8_t> Firmware(unsigned profile) {
  auto firmware = snes::tests::ArmFirmware(profile);
  if (profile == 4) {
    const uint32_t words[]{0xe3a0020e, 0xe3a01013, 0xe3a02027, 0xe880ffff,
                           0xe8907ffe, 0xe0838004, 0xe5808040, 0xeafffffa};
    for (unsigned index = 0; index < std::size(words); ++index)
      for (unsigned byte = 0; byte < 4; ++byte)
        firmware[index * 4 + byte] = uint8_t(words[index] >> (byte * 8));
  }
  return firmware;
}
void Observe(St018Processor &first, St018Processor &second) {
  Check(Capture(first) == Capture(second),
        "Restored ARM pipeline, timers and bus journal continue exactly");
  Check(std::equal(first.RamData().begin(), first.RamData().end(), second.RamData().begin()),
        "Restored ARM stores preserve all physical work RAM");
  for (uint32_t port : {0x3804u, 0x3800u, 0x3802u, 0x3804u})
    Check(first.Read(port) == second.Read(port),
          "Mailbox reads and acknowledgements stay on the same clock");
}
void Continue(St018Processor &first, St018Processor &second) {
  Observe(first, second);
  for (uint32_t clocks : {0u, 1u, 2u, 7u, 31u, 65u}) {
    first.Write(0x3802, uint8_t(clocks + 0x37));
    second.Write(0x3802, uint8_t(clocks + 0x37));
    first.Advance(clocks, clocks & 1);
    second.Advance(clocks, clocks & 1);
    Observe(first, second);
  }
}
void ProcessorCuts() {
  for (bool pal : {false, true})
    for (unsigned profile = 0; profile < 5; ++profile) {
      const auto firmware = Firmware(profile);
      St018Processor target(firmware);
      target.SetPal(pal);
      target.Advance(70000, true);
      for (unsigned cut = 0; cut < 256; ++cut) {
        St018Processor source(firmware);
        source.SetPal(pal);
        const auto frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
        source.Advance(uint32_t(uint64_t(65536) * frequency / 21'440'000), true);
        source.Advance(cut, true);
        const auto saved = Capture(source);
        const auto *allocation = target.RamData().data();
        Restore(target, saved);
        Check(Capture(target) == saved && target.RamData().data() == allocation,
              "Capture never advances ARM hardware and restore retains its mapped allocation");
        Continue(source, target);
      }
      Truncation(target);
    }
}
void InstructionFamilies() {
  const uint32_t opcodes[]{0xe0808002, 0xe0b28003, 0xe0528003, 0xe0228003, 0xe1a08213, 0xe1928003,
                           0xe1b081e2, 0xe1520003, 0x00828003, 0x10828003, 0xe5802001, 0xe5908001,
                           0xe5c02003, 0xe5d08003, 0xe1c020b2, 0xe1d080b2, 0xe1d080d3, 0xe1d080f2,
                           0xe1008092, 0xe1408092, 0xe8a07ffe, 0xe8907ffe, 0xe9007ffe, 0xe9107ffe,
                           0xe0080392, 0xe0286392, 0xe0889392, 0xe0c89392, 0xe10f8000, 0xe14f8000,
                           0xe321f0d1, 0xe361f0d2, 0xef000000, 0xee000010, 0xe12fff16, 0xeb000000};
  for (bool pal : {false, true})
    for (auto opcode : opcodes) {
      auto firmware = Firmware(0);
      const uint32_t program[]{0xe3a0020e, 0xe3a01101, 0xe3a02087, 0xe3a03029, 0xe5802000,
                               0xe3a04030, 0xe3a0507f, 0xe3a06001, opcode,     0xeafffffe};
      for (unsigned index = 0; index < std::size(program); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
          firmware[index * 4 + byte] = uint8_t(program[index] >> (byte * 8));
      St018Processor target(firmware);
      target.SetPal(pal);
      const auto frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
      const auto boot = uint32_t(uint64_t(65536) * frequency / 21'440'000);
      for (unsigned cut = 0; cut < 40; ++cut) {
        St018Processor source(firmware);
        source.SetPal(pal);
        source.Advance(boot + cut, true);
        Restore(target, Capture(source));
        Continue(source, target);
      }
    }
}
void StartupAndDebt() {
  for (bool pal : {false, true})
    for (bool held : {false, true})
      for (uint32_t cut :
           {0u, 1u, 2u, 4095u, 4096u, 4097u, 65000u, 65534u, 65535u, 65536u, 65537u, 70000u}) {
        const auto firmware = Firmware(1);
        St018Processor first(firmware), second(firmware);
        first.SetPal(pal);
        second.SetPal(pal);
        if (held)
          first.Write(0x3804, 1);
        first.Advance(cut, false);
        Restore(second, Capture(first));
        first.Write(0x3804, 0);
        second.Write(0x3804, 0);
        for (uint32_t clocks : {0u, 1u, 4096u, 4097u, 65535u, 1u, 1u, 63u}) {
          first.Advance(clocks, true);
          second.Advance(clocks, true);
          Observe(first, second);
        }
        first.Write(0x3804, 1);
        first.Advance(3, true);
        Restore(second, Capture(first));
        first.Write(0x3804, 0);
        second.Write(0x3804, 0);
        Continue(first, second);
      }
}
void InvalidContinuation() {
  constexpr size_t coreBytes = (16 + 7 + 8 + 5 + 1 + 3) * 4 + 3;
  constexpr size_t current = 4 + 32, phase = current + coreBytes, count = phase + 1 + coreBytes,
                   events = count + 4;
  const auto firmware = Firmware(4);
  St018Processor first(firmware), second(firmware);
  first.Advance(70001, true);
  second.Advance(71003, true);
  auto bad = Capture(first);
  bad[phase] = 255;
  Reject(second, bad);
  bad = Capture(first);
  bad[count] = 33;
  Reject(second, bad);
  bad = Capture(first);
  bad[events] = 255;
  Reject(second, bad);
  bad = Capture(first);
  bad[current + 60] ^= 4;
  Reject(second, bad);
  bad = Capture(first);
  const unsigned accesses = bad[count];
  Check(accesses > 0 && accesses <= 32, "Malformed-state fixture retains an active access");
  bad[events + (accesses - 1) * 17 + 13] = 1;
  Reject(second, bad);
  auto changed = firmware;
  changed[0x20000] ^= 1;
  St018Processor other(changed);
  Reject(other, Capture(first));
  second.SetPal(true);
  Reject(second, Capture(first));
  second.SetPal(false);
  Restore(second, Capture(first));
  bad = Capture(first);
  bad[events + 5] ^= 1;
  Reject(second, bad);
  Continue(first, second);
}
std::vector<uint8_t> PortableFixture() {
  StateArchive archive;
  for (bool pal : {false, true})
    for (unsigned profile = 0; profile < 5; ++profile) {
      St018Processor chip(Firmware(profile));
      chip.SetPal(pal);
      chip.Write(0x3802, 0x39);
      const auto frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
      chip.Advance(uint32_t(uint64_t(65536) * frequency / 21'440'000), true);
      for (unsigned cut = 0; cut < 24; ++cut) {
        chip.Advance(1, true);
        chip.SerializeState(archive);
      }
    }
  return archive.Bytes();
}
} // namespace
int main() {
  try {
    CorePipeline();
    ProcessorCuts();
    InstructionFamilies();
    StartupAndDebt();
    InvalidContinuation();
    Check(PortableFixture() == PortableFixture(), "Portable ARM fixture is deterministic");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  std::puts("ARM pipeline and suspended ST018 continuation checks passed");
  return 0;
}
