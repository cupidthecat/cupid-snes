#include "Hotkeys.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
unsigned OwnModifier(SDL_Scancode key) {
  switch (key) {
  case SDL_SCANCODE_LSHIFT:
  case SDL_SCANCODE_RSHIFT:
    return KeyChord::Shift;
  case SDL_SCANCODE_LCTRL:
  case SDL_SCANCODE_RCTRL:
    return KeyChord::Control;
  case SDL_SCANCODE_LALT:
  case SDL_SCANCODE_RALT:
    return KeyChord::Alt;
  case SDL_SCANCODE_LGUI:
  case SDL_SCANCODE_RGUI:
    return KeyChord::Gui;
  default:
    return 0;
  }
}
} // namespace
KeyChord KeyChord::From(const SDL_KeyboardEvent &key) {
  unsigned modifiers = 0;
  if (key.mod & SDL_KMOD_SHIFT)
    modifiers |= Shift;
  if (key.mod & SDL_KMOD_CTRL)
    modifiers |= Control;
  if (key.mod & SDL_KMOD_ALT)
    modifiers |= Alt;
  if (key.mod & SDL_KMOD_GUI)
    modifiers |= Gui;
  return {key.scancode, modifiers & ~OwnModifier(key.scancode)};
}
std::string KeyChord::Label() const {
  if (scancode == SDL_SCANCODE_UNKNOWN)
    return {};
  std::string text;
  if (modifiers & Control)
    text += "Ctrl+";
  if (modifiers & Alt)
    text += "Alt+";
  if (modifiers & Shift)
    text += "Shift+";
  if (modifiers & Gui)
    text += "Super+";
  text += SDL_GetScancodeName(scancode);
  return text;
}
Hotkeys::Hotkeys() {
  const auto set = [&](DesktopCommand command, SDL_Scancode key, unsigned mods = 0,
                       size_t slot = 0) { bindings[size_t(command)][slot] = {key, mods}; };
  set(DesktopCommand::Open, SDL_SCANCODE_O, KeyChord::Control);
  set(DesktopCommand::Unload, SDL_SCANCODE_W, KeyChord::Control);
  set(DesktopCommand::Quit, SDL_SCANCODE_Q, KeyChord::Control);
  set(DesktopCommand::Quit, SDL_SCANCODE_ESCAPE, 0, 1);
  set(DesktopCommand::Pause, SDL_SCANCODE_P);
  set(DesktopCommand::FrameAdvance, SDL_SCANCODE_N);
  set(DesktopCommand::Reset, SDL_SCANCODE_F5);
  set(DesktopCommand::Fullscreen, SDL_SCANCODE_F11);
  set(DesktopCommand::Fullscreen, SDL_SCANCODE_RETURN, KeyChord::Alt, 1);
  set(DesktopCommand::Screenshot, SDL_SCANCODE_F12);
  set(DesktopCommand::Preferences, SDL_SCANCODE_COMMA, KeyChord::Control);
}
std::optional<DesktopCommand> Hotkeys::Match(const SDL_KeyboardEvent &key) const {
  if (key.type != SDL_EVENT_KEY_DOWN || key.repeat || key.scancode == SDL_SCANCODE_UNKNOWN)
    return {};
  const auto chord = KeyChord::From(key);
  for (size_t command = 0; command < bindings.size(); ++command)
    if (std::find(bindings[command].begin(), bindings[command].end(), chord) !=
        bindings[command].end())
      return DesktopCommand(command);
  return {};
}
std::string Hotkeys::Label(DesktopCommand command) const {
  std::string label;
  for (const auto &chord : bindings.at(size_t(command))) {
    const auto text = chord.Label();
    if (text.empty())
      continue;
    if (!label.empty())
      label += " / ";
    label += text;
  }
  return label;
}
void Hotkeys::Validate() const {
  std::map<std::pair<int, unsigned>, size_t> assigned;
  for (size_t command = 0; command < bindings.size(); ++command) {
    for (const auto &chord : bindings[command]) {
      if (chord.scancode < SDL_SCANCODE_UNKNOWN || chord.scancode >= SDL_SCANCODE_COUNT ||
          chord.modifiers > 15 || (chord.modifiers & OwnModifier(chord.scancode)) ||
          (chord.scancode == SDL_SCANCODE_UNKNOWN && chord.modifiers))
        throw std::invalid_argument("Hotkey contains an invalid key or modifier");
      if (chord.scancode == SDL_SCANCODE_UNKNOWN)
        continue;
      const auto [entry, inserted] =
          assigned.emplace(std::pair{int(chord.scancode), chord.modifiers}, command);
      if (!inserted && entry->second != command)
        throw std::invalid_argument(chord.Label() + " is assigned to both " +
                                    desktopCommands[entry->second].label + " and " +
                                    desktopCommands[command].label);
    }
  }
}
std::optional<DesktopCommand> HotkeyDispatcher::Process(const Hotkeys &hotkeys,
                                                        const SDL_KeyboardEvent &key) {
  if (key.type == SDL_EVENT_KEY_DOWN) {
    if (key.repeat) {
      if (!OwnModifier(key.scancode))
        pendingModifier_.reset();
      return {};
    }
    pendingModifier_.reset();
    if (OwnModifier(key.scancode)) {
      pendingModifier_ = key;
      return {};
    }
    return hotkeys.Match(key);
  }
  if (key.type == SDL_EVENT_KEY_UP && pendingModifier_ &&
      pendingModifier_->scancode == key.scancode) {
    const auto pressed = *pendingModifier_;
    pendingModifier_.reset();
    return hotkeys.Match(pressed);
  }
  return {};
}
} // namespace snes::frontend
