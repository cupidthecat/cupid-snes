#include "gameboy/StateFixture.hpp"
#include "snes/core/FilePath.hpp"

#include <cstdio>
#include <fstream>
#include <limits>

using namespace snes::core::gameboy;
using snes::core::detail::StateArchive;
using namespace snes::tests::handheld;
namespace {
const char *stage = "startup";
uint64_t position = 0;
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
template <class Device> void Reject(Device &device, std::span<const uint8_t> bytes) {
  const auto before = Capture(device);
  bool rejected = false;
  try {
    Restore(device, bytes);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, "Invalid handheld state must reject");
  Check(Capture(device) == before, "Rejected handheld state preserves live execution and storage");
}
template <class Device> void Truncation(Device &device) {
  const auto bytes = Capture(device);
  const auto stride = std::max(size_t{1}, bytes.size() / 17);
  for (size_t length = 0; length < bytes.size(); length += stride)
    Reject(device, std::span(bytes).first(length));
  Reject(device, std::span(bytes).first(bytes.size() - 1));
  auto extra = bytes;
  extra.push_back(0);
  Reject(device, extra);
}
struct CpuFixture final : Cpu::Bus {
  std::array<uint8_t, 65536> memory{};
  uint64_t clocks = 0, reads = 0, writes = 0, trace = 14695981039346656037ull;
  Cpu cpu{*this};
  CpuFixture() {
    for (unsigned n = 0; n < memory.size(); ++n)
      memory[n] = uint8_t(n * 17 + 31);
    memory[0xffff] = memory[0xff0f] = 0;
    Cpu::State state{.a = 0x31,
                     .b = 0x81,
                     .c = 0x43,
                     .d = 0xc2,
                     .h = 0xc1,
                     .l = 0x20,
                     .sp = 0xd000,
                     .pc = 0x100};
    cpu.SetState(state);
  }
  uint8_t Read(uint16_t address) override {
    ++reads;
    Hash(trace, address);
    Hash(trace, memory[address]);
    return memory[address];
  }
  void Write(uint16_t address, uint8_t value) override {
    ++writes;
    Hash(trace, 0x10000u | address);
    Hash(trace, value);
    memory[address] = value;
  }
  void Advance(unsigned elapsed) override {
    clocks += elapsed;
  }
  uint8_t InterruptEnable() const override {
    return memory[0xffff];
  }
  uint8_t InterruptFlags() const override {
    return memory[0xff0f];
  }
  void AcknowledgeInterrupt(uint8_t mask) override {
    memory[0xff0f] &= uint8_t(~mask);
  }
  void DriveAddress(uint16_t address) override {
    Hash(trace, 0x20000u | address);
  }
  void AddressIncremented(uint16_t address) override {
    Hash(trace, 0x30000u | address);
  }
  void SerializeState(StateArchive &archive) {
    cpu.SerializeState(archive);
    archive.Array(memory);
    archive.Number(clocks);
    archive.Number(reads);
    archive.Number(writes);
    archive.Number(trace);
  }
};
uint64_t CpuStates() {
  stage = "CPU";
  uint64_t count = 0;
  for (unsigned flags : {0u, 0xf0u})
    for (unsigned opcode = 0; opcode < 512; ++opcode) {
      position = opcode + flags * 512;
      CpuFixture first, second;
      first.memory[0x100] = opcode < 256 ? uint8_t(opcode) : uint8_t(0xcb);
      first.memory[0x101] = opcode < 256 ? uint8_t(0x40) : uint8_t(opcode);
      first.memory[0x102] = 0xc0;
      auto state = first.cpu.GetState();
      state.f = uint8_t(flags);
      state.ime = opcode & 1;
      state.enablePending = opcode & 2;
      first.cpu.SetState(state);
      for (unsigned cut = 0; cut < 3; ++cut) {
        const auto saved = Capture(first);
        const auto reads = first.reads, writes = first.writes, clocks = first.clocks;
        Check(Capture(first) == saved && first.reads == reads && first.writes == writes &&
                  first.clocks == clocks,
              "CPU capture never touches its bound bus");
        Restore(second, saved);
        for (unsigned future = 0; future < 3; ++future) {
          first.memory[0xffff] = second.memory[0xffff] = uint8_t(future & 1);
          first.memory[0xff0f] = second.memory[0xff0f] = uint8_t(future & 1);
          first.cpu.RunInstruction();
          second.cpu.RunInstruction();
          Check(Capture(first) == Capture(second),
                "All normal and extended CPU opcodes retain interrupts, flags and bus effects");
        }
        Restore(first, saved);
        first.cpu.RunInstruction();
        ++count;
      }
    }
  CpuFixture invalid;
  auto bad = Capture(invalid);
  bad[1] = 1;
  Reject(invalid, bad);
  Truncation(invalid);
  return count;
}
struct TimerFixture final : ClockedIo::Bus {
  uint64_t interrupts = 0, edges = 0, trace = 0;
  ClockedIo timer{*this};
  void RequestInterrupt(uint8_t mask) override {
    interrupts += mask;
  }
  void AudioDividerEdge(bool rising) override {
    ++edges;
    Hash(trace, rising);
  }
  TimerFixture() {
    timer.Reset();
    timer.Write(0xff06, 0xfb);
    timer.Write(0xff05, 0xfe);
    timer.Write(0xff07, 5);
    timer.Write(0xff01, 0x35);
    timer.Write(0xff02, 0x81);
  }
  void SerializeState(StateArchive &archive) {
    timer.SerializeState(archive);
    archive.Number(interrupts);
    archive.Number(edges);
    archive.Number(trace);
  }
};
uint64_t TimerStates() {
  stage = "timer";
  TimerFixture first, second;
  for (unsigned cut = 0; cut < 8192; ++cut) {
    position = cut;
    const auto saved = Capture(first);
    Restore(second, saved);
    if (cut % 7 == 3) {
      first.timer.Write(0xff04, 0);
      second.timer.Write(0xff04, 0);
    }
    if (cut % 11 == 3) {
      first.timer.EnterStop(cut & 1);
      second.timer.EnterStop(cut & 1);
      first.timer.Advance(17);
      second.timer.Advance(17);
      first.timer.LeaveStop();
      second.timer.LeaveStop();
    }
    for (unsigned clocks : {0u, 1u, 3u, 7u, 31u}) {
      first.timer.Advance(clocks);
      second.timer.Advance(clocks);
      Check(Capture(first) == Capture(second),
            "Timer reload, serial bit, STOP and divider edges resume at the same clock");
    }
    Restore(first, saved);
    first.timer.Advance(1);
  }
  Truncation(first);
  auto bad = Capture(first);
  bad[2] = bad[3] = bad[4] = bad[5] = 0;
  Reject(first, bad);
  return 8192;
}
struct MapperFixture {
  uint64_t now = 1700000000, timeReads = 0;
  Cartridge cartridge;
  std::vector<uint8_t> image;
  MapperFixture(uint8_t type, unsigned variant = 0) : image(Image(type, variant)) {
    cartridge.SetTimeSource([this] {
      ++timeReads;
      return now;
    });
    cartridge.SetCameraEntropy(0x5123, true);
    Check(cartridge.Load(image), "Load handheld mapper state fixture");
    cartridge.SetClockRate(32);
  }
  void Edit(unsigned step) {
    now = 1700000000 + step * 131;
    cartridge.Write(0, 10);
    cartridge.Write(0x2000, uint8_t(step * 7));
    cartridge.Write(0x3000, uint8_t(step));
    cartridge.Write(0x4000, uint8_t(step * 3));
    cartridge.Write(0x6000, uint8_t(step & 1));
    cartridge.Write(0xa000 + (step & 511), uint8_t(step * 13));
    cartridge.SetMotion(double(step) / 7, -double(step) / 13);
    cartridge.SetInfraredInput(step & 1);
    cartridge.Advance(step % 7 + 1, step % 3 == 1);
    for (uint16_t address : {0u, 0x4000u, 0x7fffu, 0xa000u, 0xa080u, 0xbfffu})
      (void)cartridge.Read(address);
  }
  void SerializeState(StateArchive &archive) {
    cartridge.SerializeState(archive);
  }
};
uint64_t MapperStates() {
  stage = "mapper";
  uint64_t count = 0;
  for (uint8_t type : {0, 3, 6, 0x10, 0x1b, 0x1e, 0x22, 0x0d, 0xff, 0xfe, 0xfc, 0xbc}) {
    MapperFixture first(type), second(type);
    for (unsigned cut = 0; cut < 192; ++cut) {
      position = uint64_t(type) * 1000 + cut;
      first.Edit(cut);
      const auto before = first.timeReads;
      const auto saved = Capture(first);
      Check(first.timeReads == before, "Mapper capture never polls the host clock");
      const auto reads = second.timeReads;
      const auto *memory = second.cartridge.Ram().data();
      Restore(second, saved);
      Check(second.timeReads == reads && second.cartridge.Ram().data() == memory,
            "Mapper restore retains target time callback and RAM allocation");
      for (unsigned step : {cut + 1, cut + 7, cut + 31}) {
        first.Edit(step);
        second.Edit(step);
        Check(Capture(first) == Capture(second),
              "Mapper banks, clock latches, sensors and command phases continue exactly");
      }
      Restore(first, saved);
      ++count;
    }
    Truncation(first);
    auto different = first.image;
    different[0x1234] ^= 1;
    MapperFixture target(type);
    Check(target.cartridge.Load(different), "Load different same-sized handheld media");
    Reject(target, Capture(first));
  }
  for (unsigned variant : {1u, 2u}) {
    MapperFixture first(variant == 1 ? 3 : 0x10, variant), second(variant == 1 ? 3 : 0x10, variant);
    first.Edit(177);
    Restore(second, Capture(first));
    first.Edit(311);
    second.Edit(311);
    Check(Capture(first) == Capture(second), "Multicart and extended mapper controls retain state");
  }
  return count;
}
struct CameraFixture {
  Camera camera;
  uint64_t noiseState = 0x1234, noiseCalls = 0, imageHash = 0;
  explicit CameraFixture(unsigned profile) {
    camera.SetEntropy(0x739155, !(profile & 4));
    if (profile & 2)
      camera.SetNoiseSource([this] {
        ++noiseCalls;
        noiseState = noiseState * 6364136223846793005ull + 1;
        return uint8_t(noiseState >> 56);
      });
    if (profile & 1) {
      std::vector<uint8_t> pixels(128 * 112);
      for (size_t n = 0; n < pixels.size(); ++n)
        pixels[n] = uint8_t(n * 13 + n / 128);
      Check(camera.SetFrame(pixels), "Set generated camera frame");
    }
    for (unsigned n = 6; n < 54; ++n)
      camera.Write(uint16_t(n), uint8_t(32 + (n % 3) * 64));
    camera.Write(1, 0xff);
    camera.Write(2, 0xff);
    camera.Write(3, 0xff);
    camera.Write(4, 0xf0);
    camera.Write(0, 1);
  }
  void Edit(unsigned step) {
    camera.Advance(step % 3 ? 65537 : UINT32_MAX, step % 11 == 3);
    Hash(imageHash, camera.Read(0));
    for (unsigned offset : {0u, 15u, 511u, 3583u, 3584u})
      Hash(imageHash, camera.ReadImage(uint16_t(offset)));
    if (step % 7 == 3) {
      camera.Write(1, uint8_t(step * 31));
      camera.Write(2, uint8_t(step));
      camera.Write(3, uint8_t(step * 17));
      camera.Write(0, uint8_t(step & 7));
    }
    if (step % 13 == 4)
      Check(camera.SetFrame({}), "Clear camera host frame");
  }
  void SerializeState(StateArchive &archive) {
    camera.SerializeState(archive);
    archive.Number(noiseState);
    archive.Number(imageHash);
  }
};
uint64_t CameraStates() {
  stage = "camera";
  uint64_t count = 0;
  for (unsigned profile = 0; profile < 8; ++profile) {
    CameraFixture first(profile), second(profile);
    for (unsigned cut = 0; cut < 160; ++cut) {
      position = profile * 1000 + cut;
      const auto saved = Capture(first);
      const auto calls = second.noiseCalls;
      Restore(second, saved);
      Check(second.noiseCalls == calls, "Camera restore never queries its bound noise source");
      for (unsigned step : {cut, cut + 1, cut + 11}) {
        first.Edit(step);
        second.Edit(step);
        Check(Capture(first) == Capture(second),
              "Camera entropy, exposure, sleep, sensor data and cached colors resume exactly");
      }
      Restore(first, saved);
      first.Edit(cut);
      ++count;
    }
    Truncation(first);
    if (profile & 2) {
      const auto sourceCalls = first.noiseCalls, targetCalls = second.noiseCalls;
      Restore(second, Capture(first));
      second.camera.SetEntropy(1, true);
      second.camera.Write(0, 0);
      Check(first.noiseCalls == sourceCalls && second.noiseCalls == targetCalls + 1,
            "Camera noise callback stays bound to its target");
    }
  }
  return count;
}
struct MotionFixture {
  MotionEeprom device;
  std::array<uint8_t, 256> memory{};
  MotionFixture() {
    device.SetMotion(17.25, -19.75);
    memory.fill(0x5a);
  }
  void Pin(bool clock, bool input) {
    device.Write(0xa080, uint8_t(0x80 | (clock ? 0x40 : 0) | (input ? 2 : 0)), memory);
  }
  void Bit(bool input) {
    Pin(false, input);
    Pin(true, input);
  }
  void Command(uint16_t command) {
    for (int bit = 10; bit >= 0; --bit)
      Bit((command >> bit) & 1);
  }
  void SerializeState(StateArchive &archive) {
    device.SerializeState(archive);
    archive.Array(memory);
  }
};
uint64_t MotionStates() {
  stage = "EEPROM";
  uint64_t count = 0;
  for (unsigned command = 0x400; command <= 0x7ff; ++command) {
    MotionFixture first, second;
    first.Command(0x4c0);
    first.device.Write(0xa010, 0xaa, first.memory);
    for (unsigned cut = 0; cut < 43; ++cut) {
      position = command * 100 + cut;
      const bool input = cut < 11 ? (command >> (10 - cut)) & 1 : (cut * 17 + command) & 1;
      for (bool clock : {false, true}) {
        const auto saved = Capture(first);
        const auto *memory = second.memory.data();
        Restore(second, saved);
        Check(second.memory.data() == memory, "EEPROM restore retains live memory addresses");
        first.Pin(clock, input);
        second.Pin(clock, input);
        Check(first.device.Read(0xa080) == second.device.Read(0xa080) &&
                  Capture(first) == Capture(second),
              "Every EEPROM command and data edge retains pins, output and write state");
        ++count;
      }
    }
  }
  MotionFixture invalid, zero;
  invalid.device.SetMotion(std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN());
  zero.device.SetMotion(0, 0);
  invalid.device.Write(0xa010, 0xaa, invalid.memory);
  zero.device.Write(0xa010, 0xaa, zero.memory);
  Check(Capture(invalid) == Capture(zero), "Nonfinite host motion retains its zero sensor result");
  auto bad = Capture(invalid);
  const auto nan = std::bit_cast<uint64_t>(std::numeric_limits<double>::quiet_NaN());
  for (unsigned n = 0; n < 8; ++n)
    bad[n] = uint8_t(nan >> (8 * n));
  Reject(invalid, bad);
  Truncation(invalid);
  return count;
}
uint64_t ClockStates() {
  stage = "peripheral clock";
  uint64_t count = 0;
  Infrared light, targetLight;
  for (unsigned cut = 0; cut < 2048; ++cut) {
    position = cut;
    light.SetInput(cut & 1);
    light.SetOutput(cut & 2);
    const auto saved = Capture(light);
    Restore(targetLight, saved);
    for (unsigned clocks : {0u, 1u, 9949u, 10070u, UINT32_MAX}) {
      light.Advance(clocks, cut & 4);
      targetLight.Advance(clocks, cut & 4);
      Check(Capture(light) == Capture(targetLight) && light.Detected() == targetLight.Detected(),
            "Infrared charge and recovery retain full unsigned clock batches");
    }
    Restore(light, saved);
    light.Advance(cut % 5 == 3 ? UINT32_MAX : cut * 31, cut & 4);
    ++count;
  }
  Truncation(light);
  Huc3Clock clock, target;
  clock.Reset(1700000000);
  for (unsigned cut = 0; cut < 2048; ++cut) {
    position = 2048 + cut;
    clock.Write(uint8_t(cut));
    clock.Synchronize(1700000000ull + cut * 1031);
    const auto saved = Capture(clock);
    Restore(target, saved);
    for (unsigned step = 0; step < 8; ++step) {
      const auto command = uint8_t(cut * 17 + step * 31);
      clock.Write(command);
      target.Write(command);
      clock.Synchronize(1700000000ull + cut * 1031 + step * 91231);
      target.Synchronize(1700000000ull + cut * 1031 + step * 91231);
      Check(clock.Read() == target.Read() && Capture(clock) == Capture(target),
            "Clock nibbles, invalid date words, alarms and timestamps resume exactly");
    }
    Restore(clock, saved);
    ++count;
  }
  Truncation(clock);
  return count;
}
struct TppFixture {
  Tpp1 device;
  std::array<uint8_t, 32768> memory{};
  uint64_t trace = 0;
  TppFixture() {
    device.Reset(1700000000);
  }
  void Edit(unsigned step) {
    device.Write(0, uint8_t(step * 17), memory);
    device.Write(1, uint8_t(step & 8), memory);
    device.Write(2, uint8_t(step % 7), memory);
    device.Write(3, 5, memory);
    for (unsigned n = 0; n < 4; ++n)
      device.Write(uint16_t(0xa000 + n), uint8_t(step * 31 + n), memory);
    device.Write(3, 0x11, memory);
    device.Write(3, 0x19, memory);
    Hash(trace, device.Advance(step & 1 ? UINT32_MAX : step * 13, step & 2 ? UINT32_MAX : 32));
    device.Synchronize(1700000000ull + step * 91337);
    device.Write(3, uint8_t(step), memory);
    for (unsigned n = 0; n < 4; ++n) {
      const auto value = device.Read(uint16_t(0xa000 + n), memory);
      Hash(trace, value ? *value : 0x100);
    }
  }
  void SerializeState(StateArchive &archive) {
    device.SerializeState(archive);
    archive.Array(memory);
    archive.Number(trace);
  }
};
uint64_t TppStates() {
  stage = "TPP1 clock";
  TppFixture first, second;
  for (unsigned cut = 0; cut < 1024; ++cut) {
    position = cut;
    const auto saved = Capture(first);
    Restore(second, saved);
    for (unsigned step : {cut, cut + 1, cut + 256}) {
      first.Edit(step);
      second.Edit(step);
      Check(Capture(first) == Capture(second),
            "TPP1 raw clock fields, latched commands, polling debt, banks and rumble resume");
    }
    Restore(first, saved);
    first.Edit(cut);
  }
  Truncation(first);
  auto invalid = Capture(first);
  invalid[29] = 4;
  Reject(first, invalid);
  return 1024;
}
void PacketBit(Host &host, bool bit) {
  host.JoypadWrite(true, true);
  host.JoypadWrite(bit, !bit);
}
uint64_t HostStates() {
  stage = "packet host";
  uint64_t count = 0, firstResets = 0, secondResets = 0;
  Host first([&] { ++firstResets; }), second([&] { ++secondResets; });
  first.Write(0x6003, 0xb2);
  for (unsigned packet = 0; packet < 96; ++packet) {
    first.JoypadWrite(false, false);
    for (unsigned cut = 0; cut <= 128; ++cut) {
      position = packet * 1000 + cut;
      const auto saved = Capture(first);
      const auto resets = secondResets;
      Restore(second, saved);
      Check(secondResets == resets, "Host restore never invokes its reset callback");
      for (unsigned bit = 0; bit < 3; ++bit) {
        PacketBit(first, bit != 2 && ((cut + packet + bit) & 1));
        PacketBit(second, bit != 2 && ((cut + packet + bit) & 1));
        first.Pixel(uint8_t(bit));
        second.Pixel(uint8_t(bit));
        if (bit == 2) {
          first.BeginLine();
          second.BeginLine();
        }
        Check(first.Read(0x7800) == second.Read(0x7800) &&
                  first.Read(0x6002) == second.Read(0x6002) && Capture(first) == Capture(second),
              "Partial packets, queue wrap, video banks and controller locks continue exactly");
      }
      Restore(first, saved);
      PacketBit(first, cut != 128 && ((cut * 13 + packet) & 1));
      ++count;
    }
    if (packet % 3 == 1)
      (void)first.Read(0x6002);
    first.Write(0x6001, uint8_t(packet));
    first.BeginLine();
  }
  Truncation(first);
  Restore(second, Capture(first));
  first.Write(0x6003, 0);
  second.Write(0x6003, 0);
  first.Write(0x6003, 0x80);
  second.Write(0x6003, 0x80);
  Check(firstResets == 2 && secondResets == 1 && Capture(first) == Capture(second),
        "Host reset callbacks remain attached to their original targets");
  return count;
}
struct AudioFixture final : ClockedIo::Bus {
  Samples samples;
  uint64_t clock = 0, interrupts = 0;
  ClockedIo divider{*this};
  Audio audio{samples};
  explicit AudioFixture(bool secondModel) {
    divider.Reset();
    audio.Reset(secondModel);
    audio.Write(0xff26, 0x80);
    audio.Write(0xff24, 0x77);
    audio.Write(0xff25, 0xff);
    for (unsigned n = 0; n < 16; ++n)
      audio.Write(uint16_t(0xff30 + n), uint8_t(n * 17));
    for (uint16_t port : {0xff12, 0xff17, 0xff1a, 0xff21})
      audio.Write(port, 0xf3);
    for (uint16_t port : {0xff14, 0xff19, 0xff1e, 0xff23})
      audio.Write(port, 0x87);
  }
  void RequestInterrupt(uint8_t mask) override {
    interrupts += mask;
  }
  void AudioDividerEdge(bool rising) override {
    audio.DividerEdge(rising, clock);
  }
  void Edit(unsigned step) {
    audio.Write(uint16_t(0xff10 + (step * 17) % 23), uint8_t(step * 71));
    if (step % 13 == 3)
      divider.Write(0xff04, 0);
    if (step % 67 == 3) {
      divider.EnterStop(step & 1);
      audio.Stop(true);
    }
    if (step % 67 == 9) {
      divider.LeaveStop();
      audio.Stop(false);
    }
    for (unsigned n = 0; n < step % 131; ++n) {
      ++clock;
      divider.Advance(1);
      audio.AdvanceTo(clock);
    }
  }
  void SerializeState(StateArchive &archive) {
    divider.SerializeState(archive);
    audio.SerializeState(archive);
    archive.Number(clock);
    archive.Number(interrupts);
    samples.SerializeState(archive);
  }
};
uint64_t AudioStates() {
  stage = "sound history";
  uint64_t count = 0;
  for (bool secondModel : {false, true}) {
    AudioFixture first(secondModel), second(secondModel);
    for (unsigned cut = 0; cut < 2048; ++cut) {
      position = unsigned(secondModel) * 10000 + cut;
      const auto saved = Capture(first);
      const auto calls = second.samples.callbacks;
      Restore(second, saved);
      Check(second.samples.callbacks == calls, "Sound restore never emits historical audio");
      for (unsigned step : {cut, cut + 1, cut + 67}) {
        first.Edit(step);
        second.Edit(step);
        Check(Capture(first) == Capture(second),
              "Sound triggers, STOP, divider edges, DAC filters and sample clocks resume exactly");
      }
      Restore(first, saved);
      first.Edit(cut);
      ++count;
    }
    Truncation(first);
  }
  return count;
}
uint64_t MachineStates() {
  stage = "machine";
  uint64_t count = 0;
  for (unsigned model = 0; model < 2; ++model) {
    MachineStateFixture first(model), second(model);
    for (unsigned cut = 0; cut < 1024; ++cut) {
      position = model * 1024 + cut;
      first.Edit(cut);
      const auto saved = Capture(first);
      const auto callbacks = second.samples.callbacks, time = second.timeReads;
      Restore(second, saved);
      Check(second.samples.callbacks == callbacks && second.timeReads == time,
            "Machine restore never emits samples or invokes time callbacks");
      for (unsigned clocks : {0u, 1u, 3u, 7u, 31u}) {
        first.machine.AdvanceHardware(clocks);
        second.machine.AdvanceHardware(clocks);
        Check(Capture(first) == Capture(second),
              "LCD fetch, DMA, timer, startup entropy and audio history retain each clock edge");
      }
      Restore(first, saved);
      first.machine.RunInstruction();
      ++count;
    }
    Truncation(first);
    auto state = Capture(first);
    MachineStateFixture different(model ^ 1);
    Reject(different, state);
    first.machine.Reset(model, true);
    Restore(first, state);
    Restore(second, state);
    second.now = first.now;
    const auto firstResets = first.resets, secondResets = second.resets;
    first.host.Write(0x6003, 0);
    second.host.Write(0x6003, 0);
    first.host.Write(0x6003, 0x80);
    second.host.Write(0x6003, 0x80);
    Check(first.resets == firstResets + 1 && second.resets == secondResets + 1 &&
              Capture(first) == Capture(second),
          "Restored host reset callbacks retain target bindings");
  }
  return count;
}
uint64_t AdapterStates() {
  stage = "adapter";
  uint64_t count = 0;
  for (unsigned profile = 0; profile < 16; ++profile) {
    AdapterStateFixture first(profile), second(profile);
    for (unsigned cut = 0; cut < 192; ++cut) {
      position = profile * 1000 + cut;
      first.adapter.Advance(cut % 19, false);
      const auto saved = Capture(first);
      const auto callbacks = second.callbacks, time = second.timeReads;
      second.Feed(cut + 99, 31);
      const auto advancedCallbacks = second.callbacks, advancedTime = second.timeReads;
      Restore(second, saved);
      Check(second.callbacks == advancedCallbacks && second.timeReads == advancedTime &&
                second.callbacks >= callbacks && second.timeReads >= time,
            "Adapter restore retains all externally bound callbacks without invoking them");
      for (unsigned clocks : {0u, 1u, 7u, 31u, 257u}) {
        first.Feed(cut + clocks, clocks);
        second.Feed(cut + clocks, clocks);
        Check(Capture(first) == Capture(second),
              "Adapter oscillator debt, packet video and raw/mixed audio resume identically");
      }
      Restore(first, saved);
      first.Feed(cut, cut % 13 + 1);
      ++count;
    }
    Check(first.rawCount != 0, "Generated adapter guest produces audio before checkpoint cuts");
    Truncation(first);
    auto saved = Capture(first);
    first.adapter.Write(0x6003, 0);
    second.adapter.Write(0x6003, 0);
    first.adapter.Write(0x6003, 0x80);
    second.adapter.Write(0x6003, 0x80);
    Restore(first, saved);
    Restore(second, saved);
    first.Feed(1, 513);
    second.Feed(1, 513);
    Check(Capture(first) == Capture(second),
          "An adapter reset does not replace target state bindings");
  }
  return count;
}
void Patch64(std::vector<uint8_t> &bytes, size_t offset, uint64_t value) {
  for (unsigned n = 0; n < 8; ++n)
    bytes.at(offset + n) = uint8_t(value >> (8 * n));
}
void Boundaries() {
  stage = "identity and credit";
  position = 0;
  AdapterStateFixture first(0), second(0), model(1), region(2);
  const auto saved = Capture(first.adapter);
  Reject(model.adapter, saved);
  Reject(region.adapter, saved);
  second.boot[13] ^= 1;
  Check(second.adapter.Load(second.image, second.boot), "Load different boot image");
  Reject(second.adapter, saved);
  second.boot[13] ^= 1;
  Check(second.adapter.Load(second.image, second.boot), "Reload original boot image");
  Restore(second.adapter, saved);
  second.image[0x1234] ^= 1;
  Check(second.adapter.Load(second.image, second.boot), "Load different handheld program");
  Reject(second.adapter, saved);
  second.image[0x1234] ^= 1;
  Check(second.adapter.Load(second.image, second.boot), "Reload original handheld program");
  Restore(second.adapter, saved);
  Check(Capture(second.adapter) == saved, "Successful media reload invalidates cached identity");
  auto late = saved;
  late.back() = 2;
  Reject(first.adapter, late);
  Restore(second.adapter, saved);
  first.adapter.Advance(123);
  second.adapter.Advance(123);
  Check(Capture(first.adapter) == Capture(second.adapter),
        "Late rejection retains the old execution and callbacks");
  constexpr size_t creditOffset = 2 + sizeof(uint32_t);
  auto limit = saved;
  Patch64(limit, creditOffset, uint64_t(std::numeric_limits<int64_t>::max()));
  Restore(first.adapter, limit);
  bool overflow = false;
  try {
    first.adapter.Advance(1, false);
  } catch (const std::overflow_error &) {
    overflow = true;
  }
  Check(overflow && Capture(first.adapter) == limit,
        "Unrepresentable clock credit rejects before changing state");
  Patch64(limit, creditOffset, uint64_t(-int64_t{128} * 21477272));
  Restore(first.adapter, limit);
  first.adapter.Advance(0);
  Check(Capture(first.adapter) == limit, "Maximum negative clock lead remains representable");
  Patch64(limit, creditOffset, uint64_t(-int64_t{128} * 21477272 - 1));
  Reject(first.adapter, limit);
  first.adapter.Reset();
  first.adapter.Advance(UINT32_MAX, false);
  const auto large = Capture(first.adapter);
  second.adapter.Reset();
  Restore(second.adapter, large);
  Check(Capture(second.adapter) == large, "Large deferred oscillator debt round-trips exactly");
  first.now = second.now;
  first.adapter.Write(0x6003, 0x80);
  second.adapter.Write(0x6003, 0x80);
  first.adapter.Advance(7);
  second.adapter.Advance(7);
  Check(Capture(first.adapter) == Capture(second.adapter),
        "A run transition clears deferred clock debt through each target reset callback");
}
void Portable(const char *path) {
  std::vector<uint8_t> bytes;
  const auto append = [&](auto &fixture) {
    const auto saved = Capture(fixture);
    bytes.insert(bytes.end(), saved.begin(), saved.end());
  };
  for (unsigned profile = 0; profile < 16; ++profile) {
    AdapterStateFixture fixture(profile);
    for (unsigned step = 0; step < 37; ++step)
      fixture.Feed(step, step * 7 + 1);
    append(fixture);
  }
  for (unsigned profile = 0; profile < 8; ++profile) {
    CameraFixture fixture(profile);
    for (unsigned step = 0; step < 37; ++step)
      fixture.Edit(step);
    append(fixture);
  }
  for (uint8_t type : {0, 3, 6, 0x10, 0x1b, 0x1e, 0x22, 0x0d, 0xff, 0xfe, 0xfc, 0xbc}) {
    MapperFixture fixture(type);
    for (unsigned step = 0; step < 37; ++step)
      fixture.Edit(step);
    append(fixture);
  }
  MotionFixture motion;
  motion.Command(0x4c0);
  motion.Command(0x51a);
  for (unsigned n = 0; n < 7; ++n)
    motion.Bit(n & 1);
  append(motion);
  TppFixture tpp;
  for (unsigned step = 0; step < 37; ++step)
    tpp.Edit(step);
  append(tpp);
  for (bool model : {false, true}) {
    AudioFixture fixture(model);
    for (unsigned step = 0; step < 37; ++step)
      fixture.Edit(step);
    append(fixture);
  }
  std::ofstream output(snes::core::FilePath(path), std::ios::binary);
  output.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
  Check(bool(output), "Write portable handheld fixture");
  std::printf("Portable handheld fixture: %zu bytes\n", bytes.size());
}
} // namespace
int main(int argc, char **argv) {
  try {
    const auto count = CpuStates() + TimerStates() + MapperStates() + CameraStates() +
                       MotionStates() + ClockStates() + TppStates() + HostStates() + AudioStates() +
                       MachineStates() + AdapterStates();
    Boundaries();
    if (argc == 2)
      Portable(argv[1]);
    std::printf("Handheld portable state: %llu continuation checks passed\n",
                (unsigned long long)count);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s/%llu: %s\n", stage, (unsigned long long)position, error.what());
    return 1;
  }
}
