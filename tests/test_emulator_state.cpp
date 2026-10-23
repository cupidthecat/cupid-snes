#include "ConsoleConfig.hpp"
#include "audio/MsuStateFixture.hpp"
#include "snes/core/FilePath.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"
#include "state/EmulatorBoards.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
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
template <class Operation> void Reject(Operation operation, const char *message) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  Check(rejected, message);
}

std::vector<uint8_t> Rom(bool halted = false) {
  std::vector<uint8_t> rom(0x8000);
  std::fill_n(rom.begin() + 0x7fc0, 21, ' ');
  rom[0x7fc0] = 'T';
  rom[0x7fd5] = 0x20;
  rom[0x7fd6] = 2;
  rom[0x7fd7] = 5;
  rom[0x7fd8] = 3;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  const uint8_t code[]{0xa9, 0x0f, 0x8d, 0x00, 0x21, // Unblank the picture.
                       0xa9, 0x01, 0x8d, 0x00, 0x42, // Enable automatic polling.
                       0xe6, 0x00, 0xa5, 0x00, 0x8d, 0x22, 0x21, 0x8d,        0x40,
                       0x21, 0xad, 0x41, 0x21, 0x85, 0x01, 0x80, uint8_t(-17)};
  std::copy(std::begin(code), std::end(code), rom.begin());
  if (halted)
    rom[0] = 0xdb;
  return rom;
}

struct Output final : IVideoOutput, IAudioOutput {
  std::vector<std::array<uint8_t, 32>> video, audio;
  VideoFrame last;
  bool audible = false;
  void Present(const VideoFrame &frame) override {
    StateArchive archive;
    archive.Expect(frame.width);
    archive.Expect(frame.height);
    auto pixels = frame.pixels;
    archive.Vector(pixels, Ppu::OutputWidth * Ppu::OutputHeight);
    video.push_back(detail::Sha256(archive.Bytes()));
    last = frame;
  }
  void Submit(const AudioBuffer &buffer) override {
    StateArchive archive;
    archive.Expect(buffer.sampleRate);
    auto samples = buffer.interleavedStereo;
    archive.Vector(samples, 8192);
    audio.push_back(detail::Sha256(archive.Bytes()));
    audible |= std::any_of(samples.begin(), samples.end(), [](float value) { return value != 0; });
  }
};

struct Input final : IInputProvider {
  std::vector<std::array<uint64_t, 2>> polls;
  InputState Poll(uint64_t frame) override {
    polls.push_back({0, frame});
    return {.a = (frame & 1) != 0, .b = frame % 3 == 0, .start = frame % 5 == 0};
  }
  InputState PollController(int player, uint64_t frame) override {
    if (!player)
      return Poll(frame);
    polls.push_back({uint64_t(player), frame});
    return {.x = (frame + player) % 3 == 0};
  }
};

struct Machine {
  Output output;
  Input input;
  std::unique_ptr<Emulator> core;
  explicit Machine(Emulator::Config config, bool dma = false, bool headless = false)
      : core(std::make_unique<Emulator>(config)) {
    std::string error;
    Check(core->LoadCartridge(Rom(dma), &error), "Load whole-machine state fixture");
    core->AttachInputProvider(&input);
    if (!headless) {
      core->AttachVideoOutput(&output);
      core->AttachAudioOutput(&output);
    }
    auto &smp = core->GetSmp();
    smp.IoState().iplRomEnable = false;
    smp.IoState().internalWaitStates = 3;
    smp.r.pc = 0x200;
    const uint8_t code[]{0xe4, 0xf4, 0xc4, 0xf5, 0xab, 0x20, 0x2f, uint8_t(-8)};
    std::copy(std::begin(code), std::end(code), smp.Ram() + 0x200);
    auto &dsp = core->GetDsp();
    dsp.Write(Dsp::kFlg, 0x1f);
    dsp.Write(Dsp::kNon, 1);
    dsp.Write(Dsp::kMVolL, 127);
    dsp.Write(Dsp::kMVolR, 97);
    dsp.Write(0, 113);
    dsp.Write(1, 79);
    dsp.Write(7, 0x7f);
    dsp.Write(Dsp::kKon, 1);
    if (dma) {
      core->GetBus().Write(0x2100, 15);
      for (unsigned address = 0; address < MemoryBus::WramSize; ++address)
        core->GetBus().WramData()[address] = uint8_t(address * 37 + (address >> 8));
      for (unsigned channel = 0; channel < 8; ++channel) {
        auto &state = core->GetDma().Channel(channel);
        state.writeControl(0);
        state.sourceBank = 0x7e;
        state.targetAddress = 0x22;
        state.transferSize = 0;
      }
      core->GetDma().EnableDma(0xff);
    }
  }
};

void SamePicture(const VideoFrame &first, const VideoFrame &second) {
  Check(first.width == second.width && first.height == second.height &&
            first.pixels == second.pixels,
        "A checkpoint retains the last completed picture");
}

void AddPortable(StateArchive *portable, uint32_t tag, std::vector<uint8_t> bytes) {
  if (portable) {
    portable->Expect(tag);
    portable->Vector(bytes, StateArchive::MaximumBytes);
  }
}

void Continuations(StateArchive *portable) {
  for (bool pal : {false, true})
    for (bool scanline : {false, true})
      for (uint8_t revision : {1, 2})
        for (bool batch : {false, true})
          for (bool dma : {false, true}) {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.regionOverride = pal ? Region::PAL : Region::NTSC;
            config.ppu.model =
                scanline ? Ppu::RenderModel::Scanline : Ppu::RenderModel::Progressive;
            config.cpuRevision = revision;
            config.dspMode = batch ? Smp::DspMode::Batch32 : Smp::DspMode::PerCycle;
            Machine original(config, dma), control(config, dma), restored(config, dma);
            const auto initialTime = original.core->CurrentMasterCycles();
            const auto startup = original.core->CaptureState();
            restored.core->RestoreState(startup);
            Check(restored.core->CaptureState() == startup,
                  "Power-on state preserves queued reset and the initial device graph");
            Check(original.core->CurrentMasterCycles() == initialTime &&
                      original.input.polls.empty(),
                  "Capture at power-on consumes no clocks or input polls");
            original.core->StepFrame();
            const auto saved = original.core->CaptureState();
            if (revision == 2 && !batch && (!dma || !scanline))
              AddPortable(portable, unsigned(pal) + 2 * unsigned(scanline) + 4 * unsigned(dma),
                          saved);
            const auto sourcePolls = original.input.polls;
            Check(original.core->CaptureState() == saved && original.input.polls == sourcePolls,
                  "Repeated capture at a prepared boundary is passive");
            SamePicture(original.core->LastFrame(), original.output.last);
            while (control.core->CurrentFrame() < original.core->CurrentFrame())
              control.core->StepFrame();
            Check(original.output.video == control.output.video &&
                      original.output.audio == control.output.audio,
                  "Capture delivers every crossed field with unchanged picture and PCM");
            if (dma)
              Check(original.core->CurrentFrame() >= 10 && !original.core->GetDma().InDma(),
                    "Portable preparation completes long DMA while servicing all crossed fields");
            restored.core->StepFrame();
            const auto targetPolls = restored.input.polls;
            const auto targetAudio = restored.output.audio.size(),
                       targetVideo = restored.output.video.size();
            restored.core->RestoreState(saved);
            Check(restored.input.polls == targetPolls &&
                      restored.output.audio.size() == targetAudio &&
                      restored.output.video.size() == targetVideo,
                  "Restore neither polls input nor submits output");
            Check(restored.core->CaptureState() == saved,
                  "Whole-machine restore reproduces portable bytes");
            SamePicture(original.core->LastFrame(), restored.core->LastFrame());
            restored.output.video.clear();
            restored.output.audio.clear();
            const auto prefix = original.output.video.size();
            const auto future = original.core->CurrentFrame() + 3;
            while (original.core->CurrentFrame() < future) {
              const auto first = original.core->StepFrame();
              const auto second = restored.core->StepFrame();
              Check(first.frameIndex == second.frameIndex &&
                        first.masterCycles == second.masterCycles &&
                        first.latchedInput.a == second.latchedInput.a &&
                        first.latchedInput.b == second.latchedInput.b &&
                        first.latchedInput.start == second.latchedInput.start,
                    "Frame results retain counters and the already latched partial-field input");
            }
            while (control.core->CurrentFrame() < future)
              control.core->StepFrame();
            Check(original.output.video == control.output.video &&
                      original.output.audio == control.output.audio,
                  "Save preparation leaves future default picture and PCM unchanged");
            Check(original.input.polls == control.input.polls,
                  "Preparation retains every field's input and automatic polling exactly once");
            Check(std::equal(restored.output.video.begin(), restored.output.video.end(),
                             original.output.video.begin() + prefix) &&
                      std::equal(restored.output.audio.begin(), restored.output.audio.end(),
                                 original.output.audio.begin() + prefix),
                  "Restored native execution continues the partial field exactly");
            Check(original.core->CaptureState() == restored.core->CaptureState(),
                  "CPU, bus, graphics, sound, input, timing and root queues continue exactly");
            Check(original.output.audible, "The whole-machine fixture includes audible PCM");
          }
}

void HeadlessPrefix() {
  auto config = snes::tests::DeterministicConsoleConfig();
  Machine headless(config, true, true), attached(config, true);
  headless.core->StepFrame();
  attached.core->StepFrame();
  const auto saved = headless.core->CaptureState();
  Check(saved == attached.core->CaptureState(),
        "Headless capture retains the same partial audio and completed picture");
  Machine restored(config, true);
  restored.core->RestoreState(saved);
  headless.core->AttachAudioOutput(&headless.output);
  headless.core->AttachVideoOutput(&headless.output);
  headless.core->StepFrame();
  restored.core->StepFrame();
  Check(headless.output.video == restored.output.video &&
            headless.output.audio == restored.output.audio,
        "Attaching output after a headless restore preserves the entire pending field");
}

void VideoModes() {
  for (bool pal : {false, true})
    for (bool scanline : {false, true})
      for (uint8_t mode : {0, 1, 4, 5}) {
        auto config = snes::tests::DeterministicConsoleConfig();
        config.regionOverride = pal ? Region::PAL : Region::NTSC;
        config.ppu.model = scanline ? Ppu::RenderModel::Scanline : Ppu::RenderModel::Progressive;
        config.ppu.deinterlace = (mode & 1) != 0;
        if (mode == 5)
          config.ppu.vramBytes = 128 * 1024;
        Machine first(config), second(config);
        first.core->GetBus().Write(0x2133, uint8_t(mode | 8));
        first.core->StepFrame();
        first.core->StepFrame();
        const auto saved = first.core->CaptureState();
        second.core->RestoreState(saved);
        SamePicture(first.core->LastFrame(), second.core->LastFrame());
        const auto prefix = first.output.video.size();
        for (unsigned field = 0; field < 2; ++field) {
          first.core->StepFrame();
          second.core->StepFrame();
        }
        Check(std::equal(second.output.video.begin(), second.output.video.end(),
                         first.output.video.begin() + prefix) &&
                  std::equal(second.output.audio.begin(), second.output.audio.end(),
                             first.output.audio.begin() + prefix) &&
                  first.core->CaptureState() == second.core->CaptureState(),
              "Hires, interlace, overscan, deinterlacing and expanded VRAM resume exact fields");
      }
}

void InvalidAndPrepared() {
  auto config = snes::tests::DeterministicConsoleConfig();
  Machine source(config), target(config), twin(config);
  source.core->StepFrame();
  const auto saved = source.core->CaptureState();
  for (auto *machine : {&target, &twin})
    machine->core->StepFrame();
  const auto clocks = target.core->CurrentMasterCycles();
  const auto sound = target.core->GetSmp().HardwareClockCount();
  auto invalid = saved;
  invalid[0] ^= 1;
  Reject([&] { target.core->RestoreState(invalid); }, "Reject the wrong state magic");
  invalid = saved;
  invalid.back() ^= 1;
  Reject([&] { target.core->RestoreState(invalid); }, "Reject a changed state checksum");
  for (size_t size : {size_t{0}, size_t{7}, size_t{47}, saved.size() / 2, saved.size() - 1})
    Reject([&] { target.core->RestoreState(std::span<const uint8_t>(saved).first(size)); },
           "Reject truncated machine state without changing execution");
  invalid = saved;
  invalid[8] = 2;
  Reject([&] { target.core->RestoreState(invalid); }, "Reject unsupported machine state versions");
  invalid = saved;
  const auto pixels = source.core->LastFrame().pixels.size();
  const auto width = invalid.size() - pixels * 4 - 12;
  std::fill_n(invalid.begin() + width, 4, 0);
  const auto digest = detail::Sha256(std::span<const uint8_t>(invalid).subspan(48));
  std::copy(digest.begin(), digest.end(), invalid.begin() + 16);
  Reject([&] { target.core->RestoreState(invalid); }, "Reject late malformed picture metadata");
  invalid = saved;
  invalid.push_back(0);
  const auto length = uint32_t(invalid.size() - 48);
  for (unsigned byte = 0; byte < 4; ++byte)
    invalid[12 + byte] = uint8_t(length >> (byte * 8));
  const auto extraDigest = detail::Sha256(std::span<const uint8_t>(invalid).subspan(48));
  std::copy(extraDigest.begin(), extraDigest.end(), invalid.begin() + 16);
  Reject([&] { target.core->RestoreState(invalid); },
         "Reject trailing data after complete staging");
  Check(target.core->CurrentMasterCycles() == clocks &&
            target.core->GetSmp().HardwareClockCount() == sound,
        "Rejected restore leaves native continuations and clocks in place");
  for (unsigned field = 0; field < 3; ++field) {
    target.core->StepFrame();
    twin.core->StepFrame();
  }
  Check(target.output.audio == twin.output.audio && target.output.video == twin.output.video &&
            target.core->CaptureState() == twin.core->CaptureState(),
        "Late restore rejection preserves the old machine's future execution");
  auto prepared = target.core->PrepareStateRestore(saved);
  const auto undo = target.core->CaptureState();
  prepared.Commit();
  Check(target.core->CaptureState() == saved, "A prepared restore survives capture of undo state");
  Reject([&] { prepared.Commit(); }, "A prepared state commits only once");
  auto borrowed = saved;
  auto owned = target.core->PrepareStateRestore(borrowed);
  borrowed.assign(1, 0);
  owned.Commit();
  Check(target.core->CaptureState() == saved, "A prepared restore owns its entire input payload");
  target.core->RestoreState(undo);
  auto stale = target.core->PrepareStateRestore(saved);
  target.core->Reset();
  Reject([&] { stale.Commit(); }, "Reset invalidates previously prepared hardware plans");
  auto temporary = std::make_unique<Machine>(config);
  auto destroyed = temporary->core->PrepareStateRestore(saved);
  temporary.reset();
  Reject([&] { destroyed.Commit(); }, "A prepared state cannot outlive its hardware owner");
}

unsigned BoardContinuations(StateArchive *portable) {
  using namespace snes::tests::emulator_state;
  std::vector<Board> profiles;
  for (unsigned n = 0; n < unsigned(Board::Count); ++n) {
    const auto board = Board(n);
    // These mappings require private database overrides rather than a launch image.
    if (board != Board::NoMad && board != Board::Rom24 && board != Board::LargeRam)
      profiles.push_back(board);
  }
  unsigned count = 0;
  for (auto board : profiles)
    for (bool pal : {false, true}) {
      auto config = snes::tests::DeterministicConsoleConfig();
      config.regionOverride = pal ? Region::PAL : Region::NTSC;
      config.sa1BusContention = pal;
      config.cx4DelayedSync = config.superFxDelayedSync = pal;
      config.cartridgeFirmware = Firmware(board);
      auto original = std::make_unique<Emulator>(config);
      auto restored = std::make_unique<Emulator>(config);
      original->SetGameBoyTimeSource([] { return uint64_t{1700000000}; });
      restored->SetGameBoyTimeSource([] { return uint64_t{1700000000}; });
      Load(*original, board);
      Load(*restored, board);
      original->GetCpu()->Step();
      original->GetCpu()->regs().stp = true;
      if (board == Board::Handheld || board == Board::Handheld2)
        original->GetBus().Write(0x6003, 0x81);
      if (board == Board::Sa1 || board == Board::SlotSa1) {
        original->GetBus().Write(0x2203, 0);
        original->GetBus().Write(0x2204, 0x80);
        original->GetBus().Write(0x2200, 0);
      }
      Output first, second;
      original->AttachVideoOutput(&first);
      original->AttachAudioOutput(&first);
      restored->AttachVideoOutput(&second);
      restored->AttachAudioOutput(&second);
      original->StepFrame();
      auto saved = original->CaptureState();
      if (!pal &&
          (board == Board::Cx4Processor || board == Board::Handheld2 || board == Board::SlotSa1 ||
           board == Board::Competition94Processor || board == Board::Srtc))
        AddPortable(portable, 100 + unsigned(board), saved);
      restored->RestoreState(saved);
      Check(restored->CaptureState() == saved,
            "Every launch graph restores complete machine bytes");
      first.video.clear();
      first.audio.clear();
      for (unsigned field = 0; field < 2; ++field) {
        for (auto *core : {original.get(), restored.get()}) {
          auto &bus = core->GetBus();
          bus.Write(0x700123 + field, uint8_t(field * 31));
          bus.Write(0x206123 + field, uint8_t(field * 17));
          bus.Write(0x420d, uint8_t(field & 1));
          core->StepFrame();
        }
        for (auto address :
             {0x008000u, 0x808123u, 0x206123u, 0x406123u, 0x700123u, 0x004800u, 0x003000u})
          Check(original->GetBus().Read(address) == restored->GetBus().Read(address),
                "Restored cartridge ports and memory continue through the live mappings");
      }
      if (first.video != second.video || first.audio != second.audio ||
          original->CaptureState() != restored->CaptureState()) {
        std::fprintf(stderr, "Board %u, firmware %u, PAL %u\n", unsigned(board),
                     unsigned(bool(config.cartridgeFirmware)), unsigned(pal));
        throw std::runtime_error(
            "Cartridge graph and separately owned devices must continue exactly");
      }
      ++count;
    }
  return count;
}

void SeparateCommandDevices() {
  using namespace snes::tests::emulator_state;
  for (auto board : {Board::Dsp1Command, Board::Dsp2Command, Board::Srtc}) {
    auto original = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    auto restored = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    Load(*original, board);
    Load(*restored, board);
    const auto port = board == Board::Srtc ? 0x2801u : 0x308000u;
    if (board == Board::Srtc) {
      for (uint8_t value : {14, 0, 3})
        original->GetBus().Write(port, value);
    } else {
      original->GetBus().Write(port, board == Board::Dsp1Command ? 0 : 9);
      original->GetBus().Write(port, 0x34);
    }
    restored->RestoreState(original->CaptureState());
    std::vector<uint8_t> first, second;
    for (auto *core : {original.get(), restored.get()}) {
      auto &output = core == original.get() ? first : second;
      if (board == Board::Srtc) {
        for (uint8_t digit : {2, 1, 0, 4, 0, 3, 1, 5, 2, 0, 1})
          core->GetBus().Write(port, digit);
        core->GetBus().Write(port, 13);
        for (unsigned byte = 0; byte < 14; ++byte)
          output.push_back(core->GetBus().Read(0x2800));
      } else {
        for (uint8_t byte : {0x12, 0x78, 0x56})
          core->GetBus().Write(port, byte);
        for (unsigned byte = 0; byte < (board == Board::Dsp1Command ? 2u : 4u); ++byte)
          output.push_back(core->GetBus().Read(port));
      }
    }
    Check(first == second && original->CaptureState() == restored->CaptureState(),
          "Separately owned DSP and clock command halves survive a root checkpoint");
    Check(board == Board::Srtc ? first[1] == 3 : first[0] != 0x80,
          "The command fixture produces a real retained result");
  }
}

void RetainedConsoleStorage() {
  using namespace snes::tests::emulator_state;
  auto config = snes::tests::DeterministicConsoleConfig();
  config.entropyMode = EntropyMode::High;
  config.entropySeed = 0x73451;
  config.cartridgeFirmware = Firmware(Board::Cx4Processor);
  auto original = std::make_unique<Emulator>(config), restored = std::make_unique<Emulator>(config);
  original->SetGameBoyTimeSource([] { return uint64_t{1700000000}; });
  restored->SetGameBoyTimeSource([] { return uint64_t{1700000000}; });
  Load(*original, Board::Cx4Processor);
  original->GetCpu()->Step();
  original->GetCpu()->regs().stp = true;
  original->GetBus().Write(0x7f49, 0);
  original->GetBus().Write(0x7f4a, 0x80);
  original->GetBus().Write(0x7f4b, 0);
  original->GetBus().Write(0x7f48, 0);
  original->StepFrame();
  const auto storage = original->LoadedCartridge()->CaptureCx4Storage();
  Check(storage && std::any_of(storage->program[0].begin(), storage->program[0].end(),
                               [](uint16_t word) { return word != 0; }),
        "The retained console fixture fills a real processor cache");
  Load(*original, Board::Handheld);
  Load(*original, Board::Lo);
  Load(*restored, Board::Lo);
  restored->RestoreState(original->CaptureState());
  for (auto *core : {original.get(), restored.get()})
    Load(*core, Board::Cx4Processor);
  Check(original->CaptureState() == restored->CaptureState(),
        "A later cartridge load retains console-owned cache words and startup streams");
  for (auto *core : {original.get(), restored.get()})
    Load(*core, Board::Handheld);
  Check(original->CaptureState() == restored->CaptureState(),
        "An inactive handheld startup stream restores once before a later handheld load");
}

void StreamedMedia(StateArchive *portable) {
  using snes::tests::msu_state::Media;
  Media media, other;
  media.Write(media.rom, Rom());
  other.Write(other.rom, Rom());
  auto config = snes::tests::DeterministicConsoleConfig();
  auto original = std::make_unique<Emulator>(config), restored = std::make_unique<Emulator>(config);
  std::string error;
  Check(original->LoadCartridgeFromFile(PathText(media.rom), &error) &&
            restored->LoadCartridgeFromFile(PathText(other.rom), &error),
        "Load whole-machine streamed-media fixtures from independent target directories");
  Output first, second;
  original->AttachVideoOutput(&first);
  original->AttachAudioOutput(&first);
  restored->AttachVideoOutput(&second);
  restored->AttachAudioOutput(&second);
  original->GetBus().Write(0x2004, 1);
  original->GetBus().Write(0x2005, 0);
  original->GetBus().Write(0x2006, 127);
  original->GetBus().Write(0x2007, 3);
  original->StepFrame();
  const auto saved = original->CaptureState();
  AddPortable(portable, 200, saved);
  restored->RestoreState(saved);
  Check(restored->CaptureState() == saved,
        "Root restore resolves stream media from target bindings");
  first.audio.clear();
  first.video.clear();
  for (unsigned frame = 0; frame < 3; ++frame) {
    original->StepFrame();
    restored->StepFrame();
  }
  Check(first.audio == second.audio && first.video == second.video && first.audible &&
            original->CaptureState() == restored->CaptureState(),
        "Stream resources, fixed caches, mixer queues and sample clocks continue together");
  original->StepFrame();
  restored->StepFrame();
  auto pending = restored->PrepareStateRestore(saved);
  const auto clocks = restored->CurrentMasterCycles();
  std::vector<uint8_t> changed(20013);
  for (size_t n = 0; n < changed.size(); ++n)
    changed[n] = uint8_t(n * 13 + n / 127 + 0x39);
  changed.back() ^= 0x51;
  other.Write(other.Data(), changed);
  Reject([&] { pending.Commit(); }, "Changed media invalidates a prepared restore before commit");
  Check(restored->CurrentMasterCycles() == clocks,
        "A final media failure leaves the old native execution in place");
  Reject([&] { pending.Commit(); }, "A failed final media check permanently rejects its plan");
  Reject([&] { restored->RestoreState(saved); }, "Changed media also rejects a new restore");
  for (unsigned frame = 0; frame < 2; ++frame) {
    original->StepFrame();
    restored->StepFrame();
  }
  Check(first.audio == second.audio && first.video == second.video,
        "A final media rejection preserves the old suspended machine's future output");
  changed.back() ^= 0x51;
  other.Write(other.Data(), changed);
  restored->RestoreState(saved);
  Check(restored->CaptureState() == saved, "Restored matching media can recover a rejected load");
}

void ClockBindings() {
  uint64_t sourceCalls = 0, targetCalls = 0;
  auto config = snes::tests::DeterministicConsoleConfig();
  auto source = std::make_unique<Emulator>(config), target = std::make_unique<Emulator>(config);
  source->SetGameBoyTimeSource([&] {
    ++sourceCalls;
    return uint64_t{1700000000};
  });
  target->SetGameBoyTimeSource([&] {
    ++targetCalls;
    return uint64_t{1900000000};
  });
  const auto bios = snes::tests::cartridge_state::Image(0x20, 0xe3, 0x80000, "Super GAMEBOY2");
  const auto game = snes::tests::handheld::Image(0x10);
  std::string error;
  for (auto *core : {source.get(), target.get()})
    Check(core->LoadSuperGameBoy(bios, game, snes::tests::handheld::Boot(), &error),
          "Load a handheld clock through its configured host binding");
  Check(sourceCalls > 0 && targetCalls > 0, "Host clock inputs are selected before handheld load");
  const auto oldSourceCalls = sourceCalls, oldTargetCalls = targetCalls;
  const auto saved = source->CaptureState();
  auto pending = target->PrepareStateRestore(saved);
  target->SetGameBoyTimeSource([&] {
    ++targetCalls;
    return uint64_t{1950000000};
  });
  pending.Commit();
  Check(sourceCalls == oldSourceCalls && targetCalls == oldTargetCalls &&
            target->CaptureState() == saved,
        "Snapshot clocks restore without host sampling or battery elapsed-time updates");
  const auto battery = target->SaveSlotBattery(0);
  uint64_t timestamp = 0;
  Check(battery.size() >= 8, "A clocked handheld exports its battery timestamp");
  for (unsigned byte = 0; byte < 8; ++byte)
    timestamp |= uint64_t(battery[battery.size() - 8 + byte]) << (byte * 8);
  Check(timestamp == 1950000000 && targetCalls > oldTargetCalls && sourceCalls == oldSourceCalls,
        "Restoring hardware retains the target's latest host clock callback");
  unsigned emptyGameClockCalls = 0;
  auto firmwareOnly = std::make_unique<Emulator>(config);
  firmwareOnly->SetGameBoyTimeSource([&] {
    ++emptyGameClockCalls;
    return uint64_t{1700000000};
  });
  Check(firmwareOnly->LoadCartridge(bios, &error) && emptyGameClockCalls > 0,
        "A firmware-only handheld launch selects its host clock before startup reset");
}
} // namespace

int main(int argc, char **argv) {
  try {
    std::unique_ptr<StateArchive> portable;
    if (argc > 1)
      portable = std::make_unique<StateArchive>();
    Continuations(portable.get());
    HeadlessPrefix();
    VideoModes();
    InvalidAndPrepared();
    SeparateCommandDevices();
    RetainedConsoleStorage();
    ClockBindings();
    StreamedMedia(portable.get());
    const auto boards = BoardContinuations(portable.get());
    if (portable) {
      std::ofstream output(FilePath(argv[1]), std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char *>(portable->Bytes().data()),
                   std::streamsize(portable->Bytes().size()));
      output.close();
      Check(bool(output), "Write portable whole-machine states");
    }
    std::printf("Whole-machine state checks passed: %u cartridge/region graphs\n", boards);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
