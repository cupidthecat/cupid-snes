#include "ConsoleConfig.hpp"
#include "SdlInputProvider.hpp"
#include "snes/core/Emulator.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace snes::core;
using namespace snes::frontend;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct VirtualPad {
  SDL_JoystickID id;
  SDL_Joystick *joystick;
  VirtualPad() {
    SDL_VirtualJoystickDesc descriptor;
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.name = "Field input regression device";
    descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    id = SDL_AttachVirtualJoystick(&descriptor);
    Check(id != 0, "Attach field input gamepad");
    joystick = SDL_OpenJoystick(id);
    Check(joystick != nullptr, "Open field input gamepad");
    Check(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true),
          "Hold the turbo control");
    SDL_UpdateJoysticks();
  }
  ~VirtualPad() {
    SDL_CloseJoystick(joystick);
    SDL_DetachVirtualJoystick(id);
  }
};
std::vector<uint8_t> Image(unsigned channels) {
  std::vector<uint8_t> image(32768), program{0x78};
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  image[0x7fd5] = 0x20;
  image[0x7fd7] = 5;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  const auto write = [&](uint16_t address, uint8_t value) {
    program.insert(program.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
  };
  write(0x2100, 0x80);
  write(0x4200, 1);
  for (unsigned channel = 0; channel < channels; ++channel) {
    const uint16_t base = uint16_t(0x4300 + channel * 16);
    write(base, 0);
    write(base + 1, 0x22);
    write(base + 2, 0);
    write(base + 3, 0);
    write(base + 4, 0x7e);
    write(base + 5, 0);
    write(base + 6, 0);
  }
  if (channels)
    write(0x420b, uint8_t((1u << channels) - 1));
  program.push_back(0xdb);
  std::copy(program.begin(), program.end(), image.begin());
  return image;
}
InputBindings Bindings(unsigned frequency) {
  InputBindings bindings;
  bindings.turboFrequency = int(frequency);
  bindings.players[0].buttons = {};
  bindings.players[0].turbo[4][0] = {InputBinding::Kind::GamepadButton, SDL_GAMEPAD_BUTTON_SOUTH};
  return bindings;
}
struct Input final : IInputProvider {
  struct Sample {
    uint64_t frame;
    bool a;
    bool operator==(const Sample &) const = default;
  };
  SdlInputProvider provider;
  std::vector<Sample> samples;
  uint64_t completed = 0;
  explicit Input(unsigned frequency) : provider(Bindings(frequency)) {}
  InputState Poll(uint64_t frame) override {
    const auto state = provider.Poll(frame);
    samples.push_back({frame, state.a});
    return state;
  }
  void AdvanceFrame() noexcept override {
    ++completed;
    provider.AdvanceFrame();
  }
};
void Load(Emulator &machine, std::span<const uint8_t> image, IInputProvider *input) {
  std::string error;
  Check(machine.LoadCartridge(image, &error), "Load field input program");
  machine.AttachInputProvider(input);
}
void CaptureFields(bool pal, unsigned channels, unsigned frequency) {
  auto config = snes::tests::DeterministicConsoleConfig();
  config.regionOverride = pal ? Region::PAL : Region::NTSC;
  Input captured(frequency), control(frequency);
  Emulator source(config), normal(config);
  const auto image = Image(channels);
  Load(source, image, &captured);
  Load(normal, image, &control);
  source.CaptureState();
  Check(captured.completed == 0 && captured.samples.empty(),
        "Power-on capture does not tick input");
  source.StepFrame();
  const auto before = source.CurrentFrame();
  const auto saved = source.CaptureState();
  if (channels >= 2)
    Check(source.CurrentFrame() > before, "Capture crosses completed fields during DMA");
  while (normal.CurrentFrame() < source.CurrentFrame())
    normal.StepFrame();
  const auto expected = normal.CaptureState();
  Check(captured.completed == source.CurrentFrame() && control.completed == normal.CurrentFrame(),
        "Capture advances host input once for every completed emulated field");
  Check(captured.samples == control.samples,
        "Capture and normal stepping sample the same held turbo sequence");
  Check(std::all_of(captured.samples.begin(), captured.samples.end(),
                    [&](const auto &sample) {
                      return sample.a == (sample.frame % (2 * frequency) >= frequency);
                    }),
        "Turbo follows the selected released/held field cadence");
  Check(saved == expected, "Capture retains the complete machine at the same portable boundary");
  const auto ticks = captured.completed;
  const auto polls = captured.samples.size();
  Check(source.CaptureState() == saved && captured.completed == ticks &&
            captured.samples.size() == polls,
        "Repeated prepared capture is passive for host input");
  for (unsigned field = 0; field < 4 * frequency + 1; ++field) {
    const auto actual = source.StepFrame();
    const auto expectedFrame = normal.StepFrame();
    Check(actual.latchedInput.a == ((actual.frameIndex - 1) % (2 * frequency) >= frequency) &&
              actual.latchedInput.a == expectedFrame.latchedInput.a,
          "Future fields retain the expected held turbo cadence");
  }
  Check(source.CaptureState() == normal.CaptureState() && captured.samples == control.samples,
        "Future turbo input and execution remain identical after a draining capture");
  Check(source.CurrentMasterCycles() == normal.CurrentMasterCycles(),
        "Input progress consumes no hardware clocks");
}
void Ownership(bool pal) {
  auto config = snes::tests::DeterministicConsoleConfig();
  config.regionOverride = pal ? Region::PAL : Region::NTSC;
  Input original(2), target(2);
  Emulator machine(config);
  Load(machine, Image(0), &original);
  machine.StepFrame();
  Check(original.completed == 1, "Ordinary stepping notifies input without a caller tick");
  const auto saved = machine.CaptureState();
  const auto originalTicks = original.completed;
  auto prepared = machine.PrepareStateRestore(saved);
  machine.AttachInputProvider(&target);
  target.AdvanceFrame();
  target.AdvanceFrame();
  const auto targetTicks = target.completed;
  prepared.Commit();
  Check(target.completed == targetTicks && original.completed == originalTicks &&
            target.provider.PollController(0, 1000).a,
        "Restore retains the target input phase without synthetic field notifications");
  machine.StepFrame();
  Check(target.completed == targetTicks + 1 && original.completed == originalTicks,
        "Only the currently attached input receives completed fields");
  const auto beforeReset = target.completed;
  Check(machine.Reset() && machine.PowerCycle(), "Reset loaded field input hardware");
  machine.CaptureState();
  Check(target.completed == beforeReset,
        "Reset, power and passive capture retain host input phase");
  machine.AttachInputProvider(nullptr);
  machine.StepFrame();
  Check(target.completed == beforeReset, "Detached input receives no further field notifications");
}
} // namespace

int main() {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (!SDL_Init(SDL_INIT_GAMEPAD))
    return 2;
  struct Cleanup {
    ~Cleanup() {
      SDL_Quit();
    }
  } cleanup;
  try {
    VirtualPad pad;
    for (bool pal : {false, true}) {
      for (unsigned frequency : {1, 2, 3, 8})
        for (unsigned channels : {0, 1, 2, 8})
          CaptureFields(pal, channels, frequency);
      Ownership(pal);
    }
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  std::puts("Field input progress checks passed: 32 DMA/region/turbo cases and input ownership");
  return 0;
}
