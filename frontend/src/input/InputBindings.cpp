#include "InputBindings.hpp"

#include <algorithm>
#include <stdexcept>

namespace snes::frontend {
namespace {
void ValidateHost(const std::string &device, int threshold) {
  if (device.empty() || device.size() > 4096 || device.find('\0') != std::string::npos ||
      threshold < 1 || threshold > 32767)
    throw std::invalid_argument(
        "Input preferences contain an invalid gamepad selection or threshold");
}
bool Assigned(const InputAlternatives &alternatives) {
  return std::any_of(alternatives.begin(), alternatives.end(),
                     [](const auto &binding) { return binding.kind != InputBinding::Kind::None; });
}
} // namespace
void InputBinding::Validate() const {
  const bool valid =
      (kind == Kind::None && control == 0) ||
      (kind == Kind::Keyboard && control > SDL_SCANCODE_UNKNOWN && control < SDL_SCANCODE_COUNT) ||
      (kind == Kind::GamepadButton && control >= 0 && control < SDL_GAMEPAD_BUTTON_COUNT) ||
      ((kind == Kind::AxisPositive || kind == Kind::AxisNegative) && control >= 0 &&
       control < SDL_GAMEPAD_AXIS_COUNT) ||
      (kind == Kind::MouseButton && control >= 1 && control <= 5);
  if (!valid)
    throw std::invalid_argument("Input binding contains an invalid control");
}
std::string InputBinding::Label() const {
  switch (kind) {
  case Kind::Keyboard: {
    const auto *name = SDL_GetScancodeName(SDL_Scancode(control));
    return name && *name ? name : "Key " + std::to_string(control);
  }
  case Kind::GamepadButton: {
    constexpr std::array names{"South",       "East",          "West",           "North",
                               "Back",        "Guide",         "Start",          "Left stick",
                               "Right stick", "Left shoulder", "Right shoulder", "D-pad up",
                               "D-pad down",  "D-pad left",    "D-pad right"};
    if (control >= 0 && control < int(names.size()))
      return "Pad " + std::string(names[control]);
    if (const auto *name = SDL_GetGamepadStringForButton(SDL_GamepadButton(control)))
      return "Pad " + std::string(name);
    break;
  }
  case Kind::AxisPositive:
  case Kind::AxisNegative: {
    constexpr std::array names{"Left stick X",  "Left stick Y", "Right stick X",
                               "Right stick Y", "Left trigger", "Right trigger"};
    if (control >= 0 && control < int(names.size()))
      return "Pad " + std::string(names[control]) + (kind == Kind::AxisPositive ? " +" : " -");
    break;
  }
  case Kind::MouseButton: {
    constexpr std::array names{"Left", "Middle", "Right", "X1", "X2"};
    if (control >= 1 && control <= int(names.size()))
      return "Mouse " + std::string(names[control - 1]);
    break;
  }
  default:
    break;
  }
  return "Unassigned";
}
PlayerBindings PlayerBindings::Defaults(size_t player) {
  PlayerBindings result;
  constexpr std::array padButtons{
      SDL_GAMEPAD_BUTTON_DPAD_UP,       SDL_GAMEPAD_BUTTON_DPAD_DOWN,
      SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
      SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_SOUTH,
      SDL_GAMEPAD_BUTTON_NORTH,         SDL_GAMEPAD_BUTTON_WEST,
      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
      SDL_GAMEPAD_BUTTON_START,         SDL_GAMEPAD_BUTTON_BACK};
  for (size_t button = 0; button < controllerButtons.size(); ++button) {
    if (player == 0)
      result.buttons[button][0] = {InputBinding::Kind::Keyboard,
                                   KeyboardMap{}.*controllerButtons[button].keyboard};
    result.buttons[button][1] = {InputBinding::Kind::GamepadButton, padButtons[button]};
    if (button < 4)
      result.buttons[button][2] = {button % 2 == 0 ? InputBinding::Kind::AxisNegative
                                                   : InputBinding::Kind::AxisPositive,
                                   button < 2 ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_LEFTX};
  }
  return result;
}
InputBindings::InputBindings() {
  for (size_t player = 0; player < players.size(); ++player)
    players[player] = PlayerBindings::Defaults(player);
}
void InputBindings::Validate() const {
  if (turboFrequency < 1 || turboFrequency > 8)
    throw std::invalid_argument("Turbo period must be between one and eight fields");
  mouse.Validate(true);
  for (const auto &gun : guns)
    gun.Validate(false);
  for (const auto &player : players) {
    ValidateHost(player.device, player.axisThreshold);
    for (size_t button = 0; button < controllerButtons.size(); ++button) {
      for (const auto *alternatives : {&player.buttons[button], &player.turbo[button]})
        for (const auto &binding : *alternatives)
          binding.Validate();
      if (!controllerButtons[button].turbo && Assigned(player.turbo[button]))
        throw std::invalid_argument("Directional controls do not have turbo bindings");
    }
  }
}
void AxisBinding::Validate() const {
  digital.Validate();
  if (int(kind) < 0 || kind > Kind::Digital || scale < 1 || scale > 400 ||
      (kind == Kind::GamepadAxis && (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)) ||
      (kind != Kind::GamepadAxis && axis != 0) ||
      (kind != Kind::Digital && digital.kind != InputBinding::Kind::None))
    throw std::invalid_argument("Analog input contains an invalid source or scale");
}
void PeripheralBindings::Validate(bool mouse) const {
  ValidateHost(device, axisThreshold);
  if (mouse && pointerAim)
    throw std::invalid_argument("Relative mouse movement cannot use absolute pointer aim");
  for (size_t button = 0; button < buttons.size(); ++button) {
    for (const auto *alternatives : {&buttons[button], &turbo[button]})
      for (const auto &binding : *alternatives)
        binding.Validate();
    if ((mouse && button >= mouseButtonKeys.size()) || (!mouse && button == 5)) {
      if ((mouse && Assigned(buttons[button])) || Assigned(turbo[button]))
        throw std::invalid_argument("Peripheral contains an unsupported button or turbo binding");
    }
  }
  for (const auto &axis : axes)
    for (const auto &binding : axis)
      binding.Validate();
}
PeripheralBindings PeripheralBindings::Mouse() {
  PeripheralBindings result;
  result.buttons[0][0] = {InputBinding::Kind::MouseButton, SDL_BUTTON_LEFT};
  result.buttons[1][0] = {InputBinding::Kind::MouseButton, SDL_BUTTON_RIGHT};
  result.axes[0][0].kind = AxisBinding::Kind::MouseX;
  result.axes[1][0].kind = AxisBinding::Kind::MouseY;
  return result;
}
PeripheralBindings PeripheralBindings::Gun(size_t gun) {
  PeripheralBindings result;
  result.pointerAim = gun == 0;
  const auto key = [&](size_t button, SDL_Scancode key) {
    result.buttons[button][0] = {InputBinding::Kind::Keyboard, key};
  };
  if (gun == 0) {
    result.buttons[0][0] = {InputBinding::Kind::MouseButton, SDL_BUTTON_LEFT};
    result.buttons[1][0] = {InputBinding::Kind::MouseButton, SDL_BUTTON_RIGHT};
    key(2, SDL_SCANCODE_T);
    key(3, SDL_SCANCODE_SPACE);
    key(4, SDL_SCANCODE_RETURN);
    key(5, SDL_SCANCODE_O);
  } else {
    key(0, SDL_SCANCODE_RCTRL);
    key(4, SDL_SCANCODE_BACKSPACE);
    key(5, SDL_SCANCODE_U);
    constexpr std::array keys{SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_I, SDL_SCANCODE_K};
    for (size_t index = 0; index < keys.size(); ++index) {
      auto &axis = result.axes[index / 2][index % 2];
      axis.kind = AxisBinding::Kind::Digital;
      axis.scale = 300;
      axis.inverted = index % 2 == 0;
      axis.digital = {InputBinding::Kind::Keyboard, keys[index]};
    }
  }
  return result;
}
void InputBindings::SetKeyboard(const KeyboardMap &keyboard) {
  for (size_t button = 0; button < controllerButtons.size(); ++button) {
    const auto key = keyboard.*controllerButtons[button].keyboard;
    players[0].buttons[button][0] = key == SDL_SCANCODE_UNKNOWN
                                        ? InputBinding{}
                                        : InputBinding{InputBinding::Kind::Keyboard, key};
  }
}
KeyboardMap InputBindings::Keyboard() const {
  KeyboardMap result;
  for (size_t button = 0; button < controllerButtons.size(); ++button) {
    const auto &binding = players[0].buttons[button][0];
    result.*controllerButtons[button].keyboard = binding.kind == InputBinding::Kind::Keyboard
                                                     ? SDL_Scancode(binding.control)
                                                     : SDL_SCANCODE_UNKNOWN;
  }
  return result;
}
} // namespace snes::frontend
