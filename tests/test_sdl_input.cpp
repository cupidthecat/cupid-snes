#include "ConsoleConfig.hpp"
#include "SdlInputProvider.hpp"

#include <SDL3/SDL.h>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void GunViewport() {
  using snes::frontend::MakeVideoViewport;
  for (unsigned height : {224u, 239u, 448u, 478u}) {
    const auto view = MakeVideoViewport(1000, 800, height);
    const auto top = view.Aim(view.x, view.y);
    const auto center = view.Aim(view.x + view.width / 2, view.y + view.height / 2);
    const auto bottom = view.Aim(view.x + view.width - 0.01f, view.y + view.height - 0.01f);
    Check(!top.offscreen && top.x == 0 && top.y == 0, "Viewport top-left maps to first dot");
    Check(!center.offscreen && center.x == 128 && center.y == int(view.visibleLines / 2),
          "Aiming uses visible rows in both progressive and interlaced modes");
    Check(!bottom.offscreen && bottom.x == 255 && bottom.y == int(view.visibleLines - 1),
          "Viewport bottom-right maps to last dot");
    Check(view.Aim(view.x - 1, view.y).offscreen && view.Aim(view.x, view.y - 1).offscreen &&
              view.Aim(view.x + view.width, view.y).offscreen &&
              view.Aim(view.x, view.y + view.height).offscreen,
          "Letterbox and excluded edges are offscreen");
    Check(view.Aim(std::numeric_limits<float>::quiet_NaN(), view.y).offscreen,
          "Invalid aim coordinates are offscreen");
  }
  const auto small = MakeVideoViewport(128, 100, 224);
  Check(small.width <= 128 && small.height <= 100 && small.Aim(64, 50).x == 128,
        "Small windows scale down without cropping aim");
  Check(MakeVideoViewport(0, 0, 224).Aim(0, 0).offscreen, "Empty viewport is offscreen");
}

void GuestBackgroundInput(snes::frontend::SdlInputProvider &input, int player,
                          unsigned expected = 0x8000) {
  std::vector<uint8_t> rom(0x8000, 0xea);
  std::fill(rom.begin() + 0x7fc0, rom.end(), 0);
  rom[0x7fd5] = 0x20;
  rom[0x7fd7] = 5;
  rom[0x7fdc] = rom[0x7fdd] = 0xff;
  rom[0x7ffd] = 0x80;
  snes::core::Emulator machine(snes::tests::DeterministicConsoleConfig());
  machine.AttachInputProvider(&input);
  Check(machine.LoadCartridge(rom), "Generated input cartridge loads");
  machine.GetAutoJoypad().Ports().Configure(0, snes::core::ControllerDevice::Gamepad,
                                            {player, 0, 0, 0});
  const auto packet = [&] {
    auto &bus = machine.GetBus();
    bus.Write(0x4016, 1);
    bus.Write(0x4016, 0);
    unsigned value = 0;
    for (int bit = 0; bit < 16; ++bit)
      value = (value << 1) | (bus.Read(0x4016) & 1);
    return value;
  };
  Check(packet() == expected, "Unfocused allowed gamepad press reaches the guest serial port");
  input.SetEnabled(false);
  Check(packet() == 0, "Blocked background input reaches the guest as a released controller");
  input.SetEnabled(true);
  Check(packet() == expected, "Reenabled input resamples the held button at the same guest frame");
}

struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick *joystick = nullptr;
  VirtualPad() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = "Controller regression device";
    id = SDL_AttachVirtualJoystick(&desc);
    Check(id != 0, "Attach virtual gamepad");
    joystick = SDL_OpenJoystick(id);
    Check(joystick != nullptr, "Open virtual gamepad");
  }
  void Disconnect() {
    if (joystick)
      SDL_CloseJoystick(joystick);
    joystick = nullptr;
    if (id)
      SDL_DetachVirtualJoystick(id);
    id = 0;
  }
  ~VirtualPad() {
    Disconnect();
  }
};

void Remapping(snes::frontend::SdlInputProvider &input, const VirtualPad &first,
               const VirtualPad &second) {
  using namespace snes::frontend;
  using Kind = InputBinding::Kind;
  const auto devices = SdlInputProvider::ConnectedGamepads();
  const auto key = [&](SDL_JoystickID id) {
    const auto found = std::find_if(devices.begin(), devices.end(),
                                    [&](const auto &device) { return device.id == id; });
    Check(found != devices.end(), "Mapped device is discoverable");
    return found->key;
  };
  Check(key(first.id) != key(second.id), "Identical virtual pads have distinct host selections");
  InputBindings mappings;
  for (auto &player : mappings.players) {
    player.device = "none";
    player.buttons = {};
  }
  for (int player = 0; player < 8; ++player) {
    mappings.players[player].device = key(second.id);
    mappings.players[player].buttons[5][3] = {Kind::GamepadButton, SDL_GAMEPAD_BUTTON_EAST};
  }
  input.SetBindings(mappings);
  for (int player = 0; player < 8; ++player)
    GuestBackgroundInput(input, player);
  auto &eighth = mappings.players[7];
  eighth.buttons[4][0] = {Kind::AxisPositive, SDL_GAMEPAD_AXIS_LEFTX};
  eighth.buttons[8][1] = {Kind::AxisNegative, SDL_GAMEPAD_AXIS_RIGHTY};
  eighth.axisThreshold = 20000;
  SDL_SetJoystickVirtualAxis(second.joystick, SDL_GAMEPAD_AXIS_RIGHTY, -20001);
  SDL_UpdateJoysticks();
  SDL_PumpEvents();
  input.SetBindings(mappings);
  auto state = input.PollController(7, 4);
  Check(state.b && state.l && !state.a && !state.right,
        "Custom alternatives replace defaults and compare both signed axes strictly");
  eighth.axisThreshold = 19999;
  input.SetBindings(mappings);
  state = input.PollController(7, 4);
  Check(state.a && state.b && state.l, "Applying a threshold resamples within the same frame");
  GuestBackgroundInput(input, 7, 0x80a0);
  eighth.device = key(first.id);
  input.SetBindings(mappings);
  Check(!input.PollController(7, 4).b, "Explicit host assignment does not merge another gamepad");
  eighth.device = "unavailable device";
  input.SetBindings(mappings);
  Check(!input.PollController(7, 4).b, "A missing selected host pad stays released");
  eighth.device = "none";
  input.SetBindings(mappings);
  Check(!input.PollController(7, 4).b,
        "Disabling a host pad preserves independent player settings");
  eighth.buttons[5][3] = {Kind::GamepadButton, SDL_GAMEPAD_BUTTON_COUNT};
  bool rejected = false;
  try {
    input.SetBindings(mappings);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  Check(rejected && input.PollController(0, 4).b,
        "Invalid live mappings leave the previous input configuration active");
  {
    std::array<std::unique_ptr<VirtualPad>, 7> additional;
    for (auto &pad : additional)
      pad = std::make_unique<VirtualPad>();
    SDL_SetJoystickVirtualButton(additional.back()->joystick, SDL_GAMEPAD_BUTTON_WEST, true);
    SDL_UpdateJoysticks();
    SDL_PumpEvents();
    InputBindings crowded;
    for (const auto &device : SdlInputProvider::ConnectedGamepads())
      if (device.id == additional.back()->id)
        crowded.players[7].device = device.key;
    Check(crowded.players[7].device != "auto", "A ninth connected pad has an explicit selection");
    crowded.players[7].buttons = {};
    crowded.players[7].buttons[5][0] = {Kind::GamepadButton, SDL_GAMEPAD_BUTTON_WEST};
    input.SetBindings(crowded);
    GuestBackgroundInput(input, 7);
  }
  SDL_SetJoystickVirtualAxis(second.joystick, SDL_GAMEPAD_AXIS_RIGHTY, 0);
  SDL_UpdateJoysticks();
  input.SetBindings(InputBindings{});
}
} // namespace

int main() {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
    return 1;
  struct Lifetime {
    ~Lifetime() {
      SDL_Quit();
    }
  } lifetime;
  try {
    GunViewport();
    {
      snes::frontend::InputBindings missing;
      missing.players[0].device = "unavailable before any host connection";
      snes::frontend::SdlInputProvider disconnected(missing);
      Check(!disconnected.PollController(0, 0).b,
            "An unavailable saved gamepad is safe before any host pad connects");
    }
    auto *window = SDL_CreateWindow("Input regression window", 256, 224, SDL_WINDOW_HIDDEN);
    Check(window && !SDL_GetKeyboardFocus(), "Owned hidden window has no keyboard focus");
    struct WindowLifetime {
      SDL_Window *window;
      ~WindowLifetime() {
        SDL_DestroyWindow(window);
      }
    } windowLifetime{window};
    VirtualPad first, second;
    snes::frontend::SdlInputProvider input;
    SDL_SetJoystickVirtualButton(first.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(second.joystick, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualAxis(second.joystick, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_UpdateJoysticks();
    SDL_PumpEvents();
    int count = 0;
    auto *ids = SDL_GetGamepads(&count);
    const auto found = ids ? std::find(ids, ids + count, first.id) : nullptr;
    const int guestPlayer = found && found != ids + count ? int(found - ids) : -1;
    SDL_free(ids);
    Check(guestPlayer >= 0 && guestPlayer < 8, "Virtual gamepad has a frontend player slot");
    GuestBackgroundInput(input, guestPlayer);
    Check(input.PollLightGun(1, 0, 0).offscreen, "Unfocused gun input is offscreen");
    int firstPlayer = -1, secondPlayer = -1;
    for (int player = 0; player < 8; ++player) {
      const auto state = input.PollController(player, 0);
      if (state.b && !state.a)
        firstPlayer = player;
      if (state.a && state.right && !state.b)
        secondPlayer = player;
    }
    Check(firstPlayer >= 0 && secondPlayer >= 0 && firstPlayer != secondPlayer,
          "Host gamepads map to independent players");
    input.SetUiCapture(true, true);
    Check(input.PollController(firstPlayer, 0).b && input.PollController(secondPlayer, 0).a,
          "Keyboard and pointer UI capture preserve independent gamepad input");
    Check(input.PollMouse(0, 0).dx == 0 && input.PollLightGun(1, 0, 0).offscreen,
          "Pointer UI capture neutralizes relative mouse and gun input");
    input.SetUiCapture(false, false);
    input.SetEnabled(false);
    Check(!input.PollController(firstPlayer, 0).b && !input.PollController(secondPlayer, 0).a &&
              input.PollLightGun(1, 0, 0).offscreen && input.PollMouse(0, 0).dx == 0,
          "Background input blocking neutralizes all attached device types");
    input.SetEnabled(true);
    Check(input.PollController(firstPlayer, 0).b && input.PollController(secondPlayer, 0).a,
          "Returning focus resamples attached devices even at the same frame index");
    SDL_SetJoystickVirtualButton(first.joystick, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(first.joystick, SDL_GAMEPAD_BUTTON_NORTH, true);
    SDL_SetJoystickVirtualButton(first.joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    SDL_SetJoystickVirtualButton(first.joystick, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualButton(first.joystick, SDL_GAMEPAD_BUTTON_BACK, true);
    SDL_UpdateJoysticks();
    SDL_PumpEvents();
    const auto changed = input.PollController(firstPlayer, 1);
    Check(changed.x && changed.l && changed.start && changed.select && !changed.b,
          "SDL face, shoulder, start and select buttons update");
    Remapping(input, first, second);
    const auto beforeDisconnect = snes::frontend::SdlInputProvider::ConnectedGamepads();
    first.Disconnect();
    SDL_PumpEvents();
    Check(!input.PollController(firstPlayer, 2).x, "Disconnected gamepad releases buttons");
    Check(input.PollController(secondPlayer, 2).a, "Disconnect preserves other player assignments");
    const auto afterDisconnect = snes::frontend::SdlInputProvider::ConnectedGamepads();
    const auto deviceKey = [&](const auto &devices) {
      for (const auto &device : devices)
        if (device.id == second.id)
          return device.key;
      return std::string{};
    };
    Check(!deviceKey(beforeDisconnect).empty() &&
              deviceKey(beforeDisconnect) == deviceKey(afterDisconnect),
          "Removing an identical pad preserves the surviving pad's explicit selection");
    VirtualPad replacement;
    SDL_SetJoystickVirtualButton(replacement.joystick, SDL_GAMEPAD_BUTTON_WEST, true);
    SDL_UpdateJoysticks();
    SDL_PumpEvents();
    Check(input.PollController(firstPlayer, 3).y, "New gamepad takes the vacant player slot");
    Check(!input.PollController(8, 3).a && !input.PollController(-1, 3).a,
          "Out-of-range player input is empty");
    std::puts("SDL controller connection and input checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s: %s\n", error.what(), SDL_GetError());
    return 1;
  }
}
