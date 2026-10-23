#include "snes/core/BsMemory.hpp"
#include "snes/core/Bsx.hpp"
#include "snes/core/EpsonRtc.hpp"
#include "snes/core/Obc1.hpp"
#include "snes/core/PowerOnState.hpp"
#include "snes/core/Sdd1.hpp"
#include "snes/core/SetaProtocol.hpp"
#include "snes/core/Spc7110Decoder.hpp"
#include "snes/core/Srtc.hpp"
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
template <class Operation> void Reject(Operation operation) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, "Malformed cartridge state must reject");
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
  Check(Capture(device) == before, "Rejected cartridge state retains memory and all device phases");
}
template <class Device> void Truncation(Device &device) {
  const auto bytes = Capture(device);
  const auto stride = bytes.size() > 1024 ? bytes.size() / 19 : 1;
  for (size_t prefix = 0; prefix < bytes.size(); prefix += stride)
    AtomicRejection(device, {bytes.begin(), bytes.begin() + prefix});
  AtomicRejection(device, {bytes.begin(), bytes.end() - 1});
  auto extra = bytes;
  extra.push_back(0);
  AtomicRejection(device, extra);
}
void Pattern(std::span<uint8_t> bytes, unsigned salt = 0) {
  uint32_t random = 0x915ed3u + salt;
  for (auto &byte : bytes) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    byte = uint8_t(random);
  }
}

constexpr std::array<uint8_t, 12> date{9, 5, 9, 5, 3, 2, 8, 2, 2, 4, 2, 10};
void Program(Srtc &rtc) {
  rtc.Write(14);
  rtc.Write(0);
  for (auto digit : date)
    rtc.Write(digit);
}
void SharpClockContinuation() {
  for (bool pal : {false, true})
    for (unsigned mode = 0; mode < 4; ++mode)
      for (unsigned cut = 0; cut <= 13; ++cut) {
        unsigned sourceCalls = 0, targetCalls = 0;
        Srtc first([&] {
          ++sourceCalls;
          return int64_t{0};
        });
        Srtc second([&] {
          ++targetCalls;
          return int64_t{3155760000};
        });
        first.SetPal(pal);
        second.SetPal(pal);
        Program(first);
        const auto frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
        first.Advance(frequency - 11);
        if (mode == 0) {
          first.Write(13);
          for (unsigned step = 0; step < cut; ++step)
            first.Read();
        } else {
          first.Write(14);
          if (mode != 1) {
            first.Write(mode == 2 ? 0 : 2);
            if (mode == 2)
              for (unsigned step = 0; step < std::min(cut, 12u); ++step)
                first.Write(date[step]);
          }
        }
        Restore(second, Capture(first));
        Check(sourceCalls == 0 && targetCalls == 0,
              "Clock restore never applies battery elapsed time");
        if (mode == 2)
          for (unsigned step = std::min(cut, 12u); step < 12; ++step) {
            first.Write(date[step]);
            second.Write(date[step]);
          }
        for (unsigned step = 0; step < 36; ++step)
          Check(first.Read() == second.Read(),
                "Serial calendar resumes its saved mode and nibble index");
        for (auto clocks : {0u, 10u, 1u, frequency - 1, 1u, frequency + 23}) {
          first.Advance(clocks);
          second.Advance(clocks);
          first.Write(13);
          second.Write(13);
          for (unsigned step = 0; step < 15; ++step)
            Check(first.Read() == second.Read(),
                  "Clock oscillator and date carries resume on the same master clock");
        }
        Check(Capture(first) == Capture(second),
              "Clock state retains the exact oscillator remainder");
        second.Save();
        Check(targetCalls == 1 && sourceCalls == 0,
              "Clock restore retains the target time provider");
      }
  Srtc first([] { return int64_t{0}; }), second([] { return int64_t{0}; });
  for (unsigned step = 0; step < 200; ++step) {
    first.Write(14);
    first.Write(0);
    first.Write(12);
  }
  Restore(second, Capture(first));
  Check(Capture(first) == Capture(second), "Interrupted nibble writes retain raw calendar carries");
  Truncation(second);
  auto bad = Capture(second);
  bad.back() = 0xff;
  AtomicRejection(second, bad);
  second.SetPal(true);
  AtomicRejection(second, Capture(first));
}

void WriteDigit(EpsonRtc &rtc, uint8_t index, uint8_t digit) {
  rtc.Write(0, 0);
  rtc.Write(0, 1);
  rtc.Write(1, 3);
  rtc.Advance(82);
  rtc.Write(1, index);
  rtc.Advance(82);
  rtc.Write(1, digit);
  rtc.Advance(82);
}
void EpsonClockContinuation() {
  for (bool pal : {false, true})
    for (bool held : {false, true})
      for (unsigned cut : {0u, 1u, 7u, 8u, 11u, 16u, 17u})
        for (uint32_t wait : {0u, 1u, 71u, 72u, 82u}) {
          unsigned calls = 0;
          EpsonRtc first([&] {
            ++calls;
            return int64_t{0};
          });
          EpsonRtc second([&] {
            ++calls;
            return int64_t{3155760000};
          });
          first.SetPal(pal);
          second.SetPal(pal);
          for (unsigned digit = 0; digit < date.size(); ++digit)
            WriteDigit(first, uint8_t(digit), date[digit]);
          WriteDigit(first, 13, held ? 3 : 2);
          const auto frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
          first.Advance(frequency + 1);
          first.Write(0, 0);
          first.Write(0, 1);
          first.Write(1, 12);
          first.Advance(82);
          first.Write(1, 11);
          first.Advance(82);
          for (unsigned step = 0; step < cut; ++step) {
            first.Read(1, 0x75);
            first.Advance(82);
          }
          first.Read(1, 0x75);
          first.Advance(wait);
          Restore(second, Capture(first));
          Check(calls == 0, "RTC checkpoint restore never queries the host clock");
          for (unsigned step = 0; step < 48; ++step) {
            for (unsigned port = 0; port < 4; ++port)
              Check(first.Read(port, 0x75) == second.Read(port, 0x75),
                    "RTC wait/readiness, IRQ clear, offset and resync bits resume exactly");
            const auto clocks = step % 5 == 0 ? frequency - 1 : step % 2 ? 1u : 81u;
            first.Advance(clocks);
            second.Advance(clocks);
            if (step == 19) {
              WriteDigit(first, 13, 2);
              WriteDigit(second, 13, 2);
            }
          }
          Check(Capture(first) == Capture(second),
                "RTC retains held ticks and serial oscillator phase");
          const auto saved = second.Save();
          uint64_t timestamp = 0;
          for (unsigned byte = 0; byte < 8; ++byte)
            timestamp |= uint64_t(saved[16 + byte]) << (byte * 8);
          Check(calls == 1 && timestamp == 3155760000,
                "RTC restore retains the target provider for later battery saves");
        }
  EpsonRtc rtc([] { return int64_t{0}; });
  Truncation(rtc);
  auto bad = Capture(rtc);
  bad[20] = 0xff;
  bad[21] = 0xff;
  bad[22] = 0xff;
  AtomicRejection(rtc, bad);
  bad = Capture(rtc);
  bad[39] = 9;
  AtomicRejection(rtc, bad);
}

struct Flash {
  std::vector<uint8_t> image;
  BsMemory chip;
  explicit Flash(size_t size = 0x100000, bool readOnly = false)
      : image(size, 0xff), chip(image, readOnly) {
    PowerOnState entropy(EntropyMode::Low, 0x19237);
    chip.InitializePageBuffers(entropy);
    chip.SetWritable(true);
  }
  void SerializeState(StateArchive &archive) {
    archive.Fixed(std::span<uint8_t>(image));
    chip.SerializeState(archive);
  }
};
using Action = std::pair<uint32_t, uint8_t>;
std::vector<Action> FlashProgram() {
  return {{0, 0x40},     {0x2345, 0xa6}, {0, 0xfb},     {0x1235, 0x12}, {0x1235, 0x34},
          {0, 0xe0},     {0, 5},         {0, 0},        {0xfe, 0x11},   {0xff, 0x22},
          {0x100, 0x33}, {0x101, 0x44},  {0x102, 0x55}, {0x103, 0x66},  {0, 0x75},
          {0, 0x0c},     {0x1200, 5},    {0x201fe, 0},  {0, 0x99},      {0, 0xd0},
          {0, 0x72},     {0, 0x75},      {0, 0x38},     {0, 0xd0},      {0, 0x72},
          {0, 0x75},     {0, 0x96},      {0, 2},        {0, 0x77},      {0x20000, 0xd0},
          {0, 0x97},     {0, 0xd0},      {0, 0x71},     {0, 0xff}};
}
void FlashContinuation() {
  const auto actions = FlashProgram();
  for (bool pal : {false, true})
    for (unsigned cut = 0; cut <= actions.size(); ++cut) {
      Flash first, second;
      first.chip.SetPal(pal);
      second.chip.SetPal(pal);
      for (unsigned step = 0; step < cut; ++step)
        first.chip.Write(actions[step].first, actions[step].second);
      auto *image = second.image.data();
      Restore(second, Capture(first));
      Check(second.image.data() == image,
            "Flash checkpoint retains the cartridge image allocation");
      for (unsigned step = cut; step < actions.size(); ++step) {
        first.chip.Write(actions[step].first, actions[step].second);
        second.chip.Write(actions[step].first, actions[step].second);
        for (auto address : {0u, 2u, 4u, 0xfeu, 0x1234u, 0x1235u, 0x20000u})
          Check(first.chip.Read(address) == second.chip.Read(address),
                "Flash queues, both page buffers and information modes continue exactly");
      }
      Check(first.image == second.image && first.image[0x1234] == 0x12 &&
                first.image[0x201ff] == 0x22 && first.chip.Metadata() == second.chip.Metadata(),
            "Byte, word and page programming retain data and physical metadata");
      Check(Capture(first) == Capture(second),
            "Flash retains command, ready-mode and page selection state");
    }
  for (bool pal : {false, true})
    for (bool synchronize : {false, true})
      for (unsigned phase = 0; phase < 4; ++phase) {
        Flash first, second;
        first.chip.SetPal(pal);
        second.chip.SetPal(pal);
        first.image[0x1234] = 0x39;
        first.image[0x10123] = 0x6d;
        first.chip.Write(0, 0x20);
        first.chip.Write(0x1234, 0xd0);
        const auto frequency = pal ? Timing::kMasterClockHzPal : Timing::kMasterClockHz;
        const auto deadline = uint32_t(uint64_t(frequency) * 3 / 10);
        if (phase >= 1)
          first.chip.Advance(1, synchronize);
        if (phase >= 2)
          first.chip.Advance(deadline - 1, synchronize);
        if (phase >= 3)
          first.chip.Advance(1, synchronize);
        Restore(second, Capture(first));
        for (auto clocks : {0u, 1u, deadline - 1, 1u, frequency}) {
          first.chip.Advance(clocks);
          second.chip.Advance(clocks);
          Check(first.image == second.image && first.chip.Read(0) == second.chip.Read(0),
                "Erase staging, waiting block and deferred clock debt resume on the same edge");
        }
        Check(first.image[0x1234] == 0xff && first.image[0x10123] == 0x6d &&
                  first.chip.Metadata() == second.chip.Metadata() &&
                  Capture(first) == Capture(second),
              "Erase completion changes one block and retains its erase count and lock state");
      }
  for (size_t size : {size_t{0}, size_t{0x200000}, size_t{0x400000}})
    for (bool readOnly : {false, true}) {
      Flash first(size, readOnly), second(size, readOnly);
      first.chip.Write(0, 0x90);
      first.chip.Advance(123456, false);
      Restore(second, Capture(first));
      Check(first.chip.Read(1, 0x35) == second.chip.Read(1, 0x35) &&
                Capture(first) == Capture(second),
            "Empty packs, larger physical block arrays and read-only packs retain their protocol");
    }
  Flash first, second;
  first.chip.Write(0, 0xe0);
  first.chip.Write(0, 3);
  Restore(second, Capture(first));
  Truncation(second);
  auto bad = Capture(second.chip);
  bad.back() = 0x7f;
  AtomicRejection(second.chip, bad);
  Flash different(0x200000);
  AtomicRejection(different.chip, Capture(first.chip));
}

template <class Device> struct RamDevice {
  std::vector<uint8_t> ram;
  Device chip;
  explicit RamDevice(size_t size = 4096) : ram(size, 0xff), chip(ram) {}
  void SerializeState(StateArchive &archive) {
    archive.Fixed(std::span<uint8_t>(ram));
    chip.SerializeState(archive);
  }
};
void ObjectContinuation() {
  for (size_t size : {size_t{0}, size_t{256}, size_t{8192}}) {
    RamDevice<Obc1> first(size), second(size);
    Pattern(first.ram);
    first.chip.Write(0x1ff5, 1);
    first.chip.Write(0x1ff6, 0x59);
    if (size >= 8192) {
      first.ram[0x1ff5] = 0;
      first.ram[0x1ff6] = 2;
    }
    auto *memory = second.ram.data();
    Restore(second, Capture(first));
    Check(memory == second.ram.data(), "Object RAM allocation stays attached");
    for (unsigned object = 0; object < 128; ++object) {
      for (unsigned port = 0x1ff0; port < 0x1ff7; ++port) {
        Check(first.chip.Read(uint16_t(port)) == second.chip.Read(uint16_t(port)),
              "Object selector restores independently from direct RAM writes");
        first.chip.Write(uint16_t(port), uint8_t(object * 11 + port));
        second.chip.Write(uint16_t(port), uint8_t(object * 11 + port));
      }
    }
    Check(Capture(first) == Capture(second),
          "Object attribute pairs and selectors continue exactly");
    Truncation(second);
    auto bad = Capture(second.chip);
    bad[8] = 1;
    AtomicRejection(second.chip, bad);
  }
}
void ProtocolContinuation() {
  for (unsigned command : {1u, 2u})
    for (unsigned cut = 0; cut < (command == 1 ? 128u : 4u); ++cut) {
      RamDevice<St011> first, second;
      first.chip.Write(0, uint8_t(command));
      for (unsigned step = 0; step < cut; ++step)
        first.chip.Write(0, uint8_t(step * 13 + 7));
      first.chip.Write(0x4000, 0x39);
      Restore(second, Capture(first));
      for (unsigned step = cut; step < (command == 1 ? 128u : 4u); ++step) {
        first.chip.Write(0, uint8_t(step * 13 + 7));
        second.chip.Write(0, uint8_t(step * 13 + 7));
      }
      Check(std::equal(first.chip.Board().begin(), first.chip.Board().end(),
                       second.chip.Board().begin()) &&
                first.chip.Read(0x4000) == second.chip.Read(0x4000),
            "Partial command packets retain board cells and private work RAM");
      Check(Capture(first) == Capture(second),
            "Board packet continuation retains caller-owned SRAM");
    }
  const std::array<Action, 11> handshake{{{0x3804, 0},
                                          {0x3804, 1},
                                          {0x3804, 0},
                                          {0x3802, 0x55},
                                          {0x3804, 0},
                                          {0x3802, 0x66},
                                          {0x3804, 0},
                                          {0x3804, 0xff},
                                          {0x3804, 0},
                                          {0x3802, 0x77},
                                          {0x3802, 0x88}}};
  for (unsigned cut = 0; cut <= handshake.size(); ++cut)
    for (unsigned reads = 0; reads < 4; ++reads) {
      RamDevice<St018> first, second;
      for (unsigned step = 0; step < cut; ++step)
        first.chip.Write(handshake[step].first, handshake[step].second);
      for (unsigned step = 0; step < reads; ++step)
        first.chip.Read(0x3804);
      Restore(second, Capture(first));
      for (unsigned step = cut; step < handshake.size(); ++step) {
        first.chip.Write(handshake[step].first, handshake[step].second);
        second.chip.Write(handshake[step].first, handshake[step].second);
        Check(first.chip.PendingOutput() == second.chip.PendingOutput() &&
                  first.chip.Read(0x3804) == second.chip.Read(0x3804),
              "Startup commands retain partial bytes, parameter waits, pass and response counts");
      }
      Check(Capture(first) == Capture(second), "Startup handshakes retain persistent RAM");
    }
  RamDevice<St011> board;
  board.chip.Write(0, 1);
  Truncation(board);
  auto bad = Capture(board.chip);
  std::fill_n(bad.end() - 4, 4, uint8_t{0xff});
  AtomicRejection(board.chip, bad);
  RamDevice<St018> startup;
  Truncation(startup);
  bad = Capture(startup.chip);
  bad.back() = 2;
  AtomicRejection(startup.chip, bad);
}

struct DecoderBoard {
  std::vector<uint8_t> rom = std::vector<uint8_t>(0x100000);
  std::array<uint8_t, 8192> ram{};
  Sdd1 chip;
  explicit DecoderBoard(unsigned mode) : chip(rom, ram) {
    Pattern(rom);
    Pattern(ram);
    rom[0x2000] = uint8_t((rom[0x2000] & 15) | (mode << 4));
    rom[0x4000] = uint8_t((rom[0x4000] & 15) | (((mode + 5) & 15) << 4));
    Setup(0, 0xc02000, 257);
    Setup(5, 0xd04000, 512);
    chip.Write(0x4800, 0x21);
    chip.Write(0x4801, 0x21);
    for (unsigned bank = 0; bank < 4; ++bank)
      chip.Write(0x4804 + bank, uint8_t(0x80 | bank));
  }
  void Setup(unsigned channel, uint32_t address, uint16_t count) {
    const auto base = 0x4300 + channel * 16;
    for (unsigned index = 0; index < 3; ++index)
      chip.ObserveDmaWrite(base + 2 + index, uint8_t(address >> (index * 8)));
    chip.ObserveDmaWrite(base + 5, uint8_t(count));
    chip.ObserveDmaWrite(base + 6, uint8_t(count >> 8));
  }
  void SerializeState(StateArchive &archive) {
    archive.Array(ram);
    chip.SerializeState(archive);
  }
};
void DecoderContinuation() {
  for (unsigned mode = 0; mode < 16; ++mode)
    for (unsigned cut : {0u, 1u, 2u, 3u, 7u, 31u, 65u, 129u, 255u, 513u}) {
      DecoderBoard first(mode), second(mode);
      for (unsigned step = 0; step < cut; ++step)
        first.chip.Read(step % 11 == 0 ? 0xd04000 : 0xc02000, 0x73);
      Restore(second, Capture(first));
      for (unsigned step = cut; step < 2048; ++step) {
        const auto address = step % 11 == 0 ? 0xd04000u : 0xc02000u;
        Check(first.chip.Read(address, 0x73) == second.chip.Read(address, 0x73),
              "Compression contexts, run state, bit history and pending odd bytes resume exactly");
        if (step == 800) {
          first.chip.Write(0x4801, 0x21);
          second.chip.Write(0x4801, 0x21);
        }
      }
      Check(Capture(first) == Capture(second),
            "Shared decoder and all DMA channel counters continue exactly");
    }
  DecoderBoard first(7), second(7);
  first.Setup(0, 0xc02000, 0);
  first.chip.Read(0xc02000, 0);
  Restore(second, Capture(first));
  for (unsigned step = 1; step < 65537; ++step)
    Check(first.chip.Read(0xc02000, 0) == second.chip.Read(0xc02000, 0),
          "Zero DMA count resumes the full 65536-byte stream and its following raw read");
  Truncation(second);
  for (auto [offset, value] : {std::pair{size_t{76}, uint8_t{9}},
                               {size_t{80}, uint8_t{32}},
                               {size_t{86}, uint8_t{33}},
                               {size_t{342}, uint8_t{129}}}) {
    auto bad = Capture(second.chip);
    bad[offset] = value;
    AtomicRejection(second.chip, bad);
  }
}

void CodingRowContinuation() {
  const auto byte = [](uint32_t address) { return uint8_t(address * 37 + (address >> 4) + 0xa5); };
  for (unsigned mode = 0; mode < 3; ++mode)
    for (unsigned cut : {0u, 1u, 2u, 7u, 8u, 15u, 16u, 31u, 65u, 129u, 511u}) {
      std::vector<uint32_t> sourceReads, targetReads;
      Spc7110Decoder first([&](uint32_t address) {
        sourceReads.push_back(address);
        return byte(address);
      });
      Spc7110Decoder second([&](uint32_t address) {
        targetReads.push_back(address);
        return byte(address);
      });
      first.Initialize(mode, 0xfff731);
      for (unsigned row = 0; row < cut; ++row)
        first.NextRow();
      const auto reads = sourceReads.size();
      Restore(second, Capture(first));
      Check(targetReads.empty() && sourceReads.size() == reads,
            "Coding state restore never reads its attached cartridge source");
      sourceReads.clear();
      for (unsigned row = cut; row < 1024; ++row)
        Check(
            first.NextRow() == second.NextRow(),
            "Coding models, interval, input bits, color order and pixel history continue exactly");
      Check(sourceReads == targetReads && !targetReads.empty() && Capture(first) == Capture(second),
            "The restored decoder retains its target reader and exact byte consumption");
    }
  Spc7110Decoder first(byte), second(byte);
  Restore(second, Capture(first));
  for (unsigned row = 0; row < 10; ++row)
    Check(first.NextRow() == second.NextRow(), "A cold decoder retains its zero color table");
  first.Initialize(2, 0x1234);
  first.NextRow();
  Restore(second, Capture(first));
  Truncation(second);
  auto bad = Capture(first);
  bad[0] = 53;
  AtomicRejection(second, bad);
  bad = Capture(first);
  bad[150] = bad[151];
  AtomicRejection(second, bad);
  bad = Capture(first);
  bad[170] = 0;
  bad[171] = 0;
  AtomicRejection(second, bad);
  bad = Capture(first);
  bad.back() = 3;
  AtomicRejection(second, bad);
}

struct BroadcastBoard {
  std::vector<uint8_t> bios = std::vector<uint8_t>(0x100000);
  std::vector<uint8_t> pack = std::vector<uint8_t>(0x100000, 0xff);
  std::array<uint8_t, 32768> ram{};
  Bsx chip;
  unsigned samples = 0;
  explicit BroadcastBoard(unsigned salt = 0) : chip(bios, pack, ram) {
    Pattern(bios);
    Pattern(ram);
    for (unsigned index = 0; index < 16; ++index)
      chip.Psram()[index * 16384] = uint8_t(0x39 + index);
    std::array<uint8_t, 57> payload;
    Pattern(payload, salt);
    chip.LoadStream(0x1234, 0, payload);
    chip.SetTimeSource([&] {
      ++samples;
      return BroadcastTime{2041, 11, 27, 4, 21, 39, 57};
    });
  }
  void Mapper(unsigned index, bool value) {
    chip.WriteCpu((index << 16) | 0x5000, value ? 0x80 : 0);
  }
  void Prefix() {
    for (unsigned index = 1; index <= 13; ++index)
      Mapper(index, index == 2 || index == 12 || index == 13);
    Mapper(14, true);
    chip.WriteCpu(0xc00000, 0xe0);
    chip.WriteCpu(0xc00000, 2);
    chip.WriteCpu(0xc00000, 0);
    chip.WriteCpu(0xc000fe, 0x11);
    Mapper(2, false);
    Mapper(3, true);
    chip.WriteCpu(0x218b, 1);
    chip.WriteCpu(0x218c, 1);
    for (unsigned step = 0; step < 17; ++step)
      chip.ReadCpu(0x218c);
  }
  void SerializeState(StateArchive &archive) {
    archive.Fixed(std::span<uint8_t>(pack));
    archive.Array(ram);
    chip.SerializeState(archive);
  }
};
void BroadcastBoardContinuation() {
  BroadcastBoard first, second;
  first.Prefix();
  const auto bytes = Capture(first);
  auto *memory = second.chip.Psram().data();
  Restore(second, bytes);
  Check(second.samples == 0 && second.chip.Psram().data() == memory,
        "Broadcast restore retains PSRAM allocation and never samples the host provider");
  first.samples = 0;
  for (unsigned step = 0; step < 32; ++step)
    Check(first.chip.ReadCpu(0x218c) == second.chip.ReadCpu(0x218c),
          "Broadcast calendar continues through the mapper");
  first.chip.WriteCpu(0xc000ff, 0x22);
  second.chip.WriteCpu(0xc000ff, 0x22);
  first.chip.WriteCpu(0xc00100, 0x33);
  second.chip.WriteCpu(0xc00100, 0x33);
  first.Mapper(14, true);
  second.Mapper(14, true);
  for (auto address : {0x008123u, 0x206123u, 0x408123u, 0xc01234u, 0x105123u}) {
    Check(first.chip.ReadCpu(address, 0x73) == second.chip.ReadCpu(address, 0x73),
          "Active and uncommitted mapper bits resume distinct cartridge windows");
    first.chip.WriteCpu(address, uint8_t(address * 37));
    second.chip.WriteCpu(address, uint8_t(address * 37));
  }
  Check(first.samples == second.samples && Capture(first) == Capture(second),
        "PSRAM, borrowed pack/SRAM, flash queues and recorded reception continue together");
  Truncation(second);
  BroadcastBoard changed(1);
  changed.pack[3] = 0x39;
  AtomicRejection(changed, bytes);
}

std::vector<uint8_t> PortableFixture() {
  Flash flash;
  flash.chip.Write(0, 0xe0);
  flash.chip.Write(0, 2);
  flash.chip.Write(0, 0);
  flash.chip.Write(0xfe, 0x39);
  BroadcastBoard broadcast;
  broadcast.Prefix();
  DecoderBoard decoder(5);
  for (unsigned step = 0; step < 31; ++step)
    decoder.chip.Read(0xc02000, 0);
  Spc7110Decoder coding(
      [](uint32_t address) { return uint8_t(address * 37 + (address >> 4) + 0xa5); });
  coding.Initialize(2, 0xfff731);
  for (unsigned row = 0; row < 71; ++row)
    coding.NextRow();
  Srtc sharp([] { return int64_t{0}; });
  Program(sharp);
  sharp.Advance(1234567);
  sharp.Write(13);
  sharp.Read();
  sharp.Read();
  EpsonRtc epson([] { return int64_t{0}; });
  WriteDigit(epson, 13, 3);
  epson.Advance(Timing::kMasterClockHz + 1);
  RamDevice<St011> board;
  board.chip.Write(0, 1);
  for (unsigned step = 0; step < 71; ++step)
    board.chip.Write(0, uint8_t(step * 17));
  RamDevice<St018> startup;
  startup.chip.Write(0x3804, 0);
  startup.chip.Write(0x3804, 1);
  RamDevice<Obc1> objects(8192);
  objects.chip.Write(0x1ff5, 0);
  objects.chip.Write(0x1ff6, 0x39);
  objects.chip.Write(0x1ff4, 3);
  StateArchive archive;
  for (unsigned pass = 0; pass < 2; ++pass) {
    flash.SerializeState(archive);
    broadcast.SerializeState(archive);
    decoder.SerializeState(archive);
    coding.SerializeState(archive);
    sharp.SerializeState(archive);
    epson.SerializeState(archive);
    board.SerializeState(archive);
    startup.SerializeState(archive);
    objects.SerializeState(archive);
    flash.chip.Write(0xff, 0x6d);
    broadcast.Mapper(14, true);
    decoder.chip.Read(0xc02000, 0);
    coding.NextRow();
    sharp.Advance(21477272);
    epson.Advance(21477272);
    board.chip.Write(0, 0x79);
    startup.chip.Write(0x3804, 0);
    objects.chip.Write(0x1ff0, 0x3f);
  }
  return archive.Bytes();
}
} // namespace

int main() {
  try {
    SharpClockContinuation();
    EpsonClockContinuation();
    FlashContinuation();
    ObjectContinuation();
    ProtocolContinuation();
    DecoderContinuation();
    CodingRowContinuation();
    BroadcastBoardContinuation();
    Check(!PortableFixture().empty(), "Cartridge checkpoint fixture captures every device");
    std::puts("Cartridge clock, flash, mapper, packet and decompression checkpoint checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
