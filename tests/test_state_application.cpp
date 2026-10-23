#include "SdlInputProvider.hpp"
#include "desktop/DesktopApplication.hpp"
#include "imgui_internal.h"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"
#include "storage/StateStorage.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;
using namespace snes::frontend;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Predicate> void Wait(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Push(SDL_Event event) {
    Check(SDL_PushEvent(&event), "Application event can be queued");
}

struct ProgressWindow {
    bool active = false;
    float x = 0, y = 0, width = 0, height = 0;
};

ProgressWindow ReadProgress() {
    ProgressWindow progress;
    Check(SDL_RunOnMainThread(
              [](void* userdata) {
                  auto& progress = *static_cast<ProgressWindow*>(userdata);
                  if (ImGui::GetCurrentContext())
                      if (const auto* window = ImGui::FindWindowByName("State operation")) {
                          progress.active = window->Active;
                          progress.x = window->Pos.x;
                          progress.y = window->Pos.y;
                          progress.width = window->Size.x;
                          progress.height = window->Size.y;
                      }
              },
              &progress, true),
          "Progress observer runs on the owning UI thread");
    return progress;
}

void Ready() {
    Wait([] { return !ReadProgress().active; },
         "State progress closes before the next interaction");
}

void Key(SDL_WindowID window, SDL_Scancode scan, SDL_Keycode key, SDL_Keymod modifiers = 0) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = window;
    event.key.scancode = scan;
    event.key.key = key;
    event.key.mod = modifiers;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    event.type = SDL_EVENT_KEY_UP;
    event.key.mod = 0;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Ready();
}

void Quit() {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    Push(event);
}

void Click(SDL_WindowID window, float x, float y, bool settle = true) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = window;
    event.motion.x = x;
    event.motion.y = y;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    event = {};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = window;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = false;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (settle)
        Ready();
}

Preferences Profile(const fs::path& path) {
    struct Request {
        const fs::path& path;
        Preferences preferences;
        std::exception_ptr failure;
    } request{path, {}, {}};

    Check(SDL_RunOnMainThread(
              [](void* userdata) {
                  auto& request = *static_cast<Request*>(userdata);
                  try {
                      request.preferences = PreferencesFile(request.path).Load();
                  } catch (...) {
                      request.failure = std::current_exception();
                  }
              },
              &request, true),
          "Profile observer runs between application writes");
    if (request.failure)
        std::rethrow_exception(request.failure);
    return request.preferences;
}

bool SDLCALL WindowCreated(void* userdata, SDL_Event* event) {
    if (event->type == SDL_EVENT_WINDOW_SHOWN)
        static_cast<std::atomic<SDL_WindowID>*>(userdata)->store(event->window.windowID);
    return true;
}

struct Picture {
    SDL_WindowID window;
    std::string path, title;
    uint32_t center = 0;
    int width = 0, height = 0;
    bool valid = false;
};

Picture ReadPicture(SDL_WindowID window, const fs::path& path = {}) {
    Picture result{window, snes::core::PathText(path), {}};
    Check(SDL_RunOnMainThread(
              [](void* userdata) {
                  auto& picture = *static_cast<Picture*>(userdata);
                  auto* window = SDL_GetWindowFromID(picture.window);
                  auto* renderer = window ? SDL_GetRenderer(window) : nullptr;
                  auto* surface = renderer ? SDL_RenderReadPixels(renderer, nullptr) : nullptr;
                  if (!surface)
                      return;
                  picture.title = SDL_GetWindowTitle(window);
                  picture.width = surface->w;
                  picture.height = surface->h;
                  Uint8 r = 0, g = 0, b = 0, a = 0;
                  picture.valid =
                      SDL_ReadSurfacePixel(surface, surface->w / 2, surface->h / 2, &r, &g, &b, &a);
                  picture.center = 0xff000000u | uint32_t(b) << 16 | uint32_t(g) << 8 | r;
                  if (!picture.path.empty())
                      picture.valid &= SDL_SaveBMP(surface, picture.path.c_str());
                  SDL_DestroySurface(surface);
              },
              &result, true),
          "Rendered picture can be read on the main thread");
    Check(result.valid, "Application has a rendered picture");
    return result;
}

class ErrorCapture : public std::streambuf {
  public:
    ErrorCapture() : original_(std::cerr.rdbuf(this)) {
    }

    ~ErrorCapture() {
        std::cerr.rdbuf(original_);
    }

    bool Contains(const std::string& text) {
        std::lock_guard lock(mutex_);
        return text_.find(text) != std::string::npos;
    }

    bool Empty() {
        std::lock_guard lock(mutex_);
        return text_.empty();
    }

  private:
    std::streamsize xsputn(const char* text, std::streamsize size) override {
        std::lock_guard lock(mutex_);
        text_.append(text, size);
        return original_->sputn(text, size);
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        const char c = traits_type::to_char_type(value);
        return xsputn(&c, 1) == 1 ? value : traits_type::eof();
    }

    std::mutex mutex_;
    std::string text_;
    std::streambuf* original_;
};

std::vector<uint8_t> Image() {
    std::vector<uint8_t> image(32768);
    std::fill_n(image.begin() + 0x7fc0, 21, ' ');
    image[0x7fd5] = 0x20;
    image[0x7fd7] = 5;
    image[0x7fd9] = 1;
    image[0x7fdc] = image[0x7fdd] = 0xff;
    image[0x7ffd] = 0x80;
    image[0x7feb] = image[0x7ffb] = 0x81;
    const uint8_t boot[] = {0x78, 0x64, 0x00, 0xa9, 0x0f, 0x8d, 0x00, 0x21,
                            0xa9, 0x80, 0x8d, 0x00, 0x42, 0xcb, 0x80, 0xfd};
    const uint8_t nmi[] = {0x48, 0xe6, 0x00, 0x9c, 0x21, 0x21, 0xa5, 0x00,
                           0x8d, 0x22, 0x21, 0x8d, 0x22, 0x21, 0x68, 0x40};
    std::copy(std::begin(boot), std::end(boot), image.begin());
    std::copy(std::begin(nmi), std::end(nmi), image.begin() + 0x100);
    return image;
}

uint64_t FrameCount(const StateFile& state, const fs::path& rom) {
    snes::core::Emulator machine;
    std::string error;
    Check(machine.LoadCartridgeFromFile(snes::core::PathText(rom), &error),
          "Independent machine opens the generated cartridge");
    machine.RestoreState(state.machine);
    return machine.CurrentFrame();
}

void Workflow(const fs::path& directory, const fs::path& pictures) {
    const auto rom = directory / "state-game.sfc";
    {
        const auto bytes = Image();
        std::ofstream out(rom, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        Check(bool(out), "State workflow cartridge writes");
    }
    const auto profile = directory / "preferences.cfg";
    PreferencesFile file(profile);
    auto preferences = file.Load();
    preferences.focus = FocusBehavior::AllowInput;
    preferences.stateDirectory = snes::core::PathText(directory / fs::path(u8"\u72b6\u614b"));
    file.Save(preferences);
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage states(request, snes::core::FilePath(preferences.stateDirectory));
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
    ErrorCapture errors;
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); },
                 "State application creates its window");
            Wait([&] { return returned.load() || Profile(profile).recent.size() == 1; },
                 "State cartridge opens in the application");
            Check(!returned, "State application stays open");
            Wait(
                [&] {
                    const auto color = ReadPicture(window).center;
                    return color != 0xff000000 && color != 0xff1e1816;
                },
                "Generated cartridge presents a completed picture before pausing");
            Key(window, SDL_SCANCODE_P, SDLK_P);
            Wait([&] { return ReadPicture(window).title.find("Paused") != std::string::npos; },
                 "State workflow pauses playback");
            const auto select = [&](unsigned slot) {
                unsigned attempts = 0;
                while (Profile(profile).stateSlot != slot) {
                    Check(++attempts <= 12 && !returned, "Quick-slot shortcut remains available");
                    Key(window, SDL_SCANCODE_F7, SDLK_F7);
                }
            };
            const auto save = [&](unsigned slot) {
                select(slot);
                Key(window, SDL_SCANCODE_F2, SDLK_F2);
                const auto path = states.Path(StateStorage::Quick(slot));
                Wait([&] { return returned.load() || fs::is_regular_file(path); },
                     "Quick state reaches disk");
                Check(!returned, "Quick save retains the running application");
                return StateFile::Read(path);
            };
            const auto first = save(0);
            Check(!first.preview.pixels.empty(), "Quick state stores the last completed picture");
            Key(window, SDL_SCANCODE_N, SDLK_N);
            const auto second = save(1);
            Check(first.machine != second.machine && first.preview.pixels != second.preview.pixels,
                  "Frame advance changes both whole-machine state and completed picture");
            select(0);
            Key(window, SDL_SCANCODE_F4, SDLK_F4);
            const auto expectedPixel = first.preview.pixels[120 * 256 + 128];
            Wait([&] { return ReadPicture(window).center == expectedPixel; },
                 "Paused restore presents the saved picture without stepping");
            Check(save(2).machine == first.machine,
                  "Quick load restores exact execution while paused");
            Key(window, SDL_SCANCODE_Z, SDLK_Z, SDL_KMOD_CTRL);
            Check(save(3).machine == second.machine,
                  "Undo hotkey restores the exact pre-load machine");
            Key(window, SDL_SCANCODE_Y, SDLK_Y, SDL_KMOD_CTRL);
            Check(save(4).machine == first.machine, "Redo hotkey restores the loaded machine");
            auto corrupt = ReadFile(states.Path(StateStorage::Quick(0)), 140 * 1024 * 1024);
            corrupt.back() ^= 1;
            WriteFileAtomically(states.Path(StateStorage::Quick(0)), corrupt);
            select(0);
            Key(window, SDL_SCANCODE_F4, SDLK_F4);
            Wait([&] { return errors.Contains("checksum"); },
                 "Malformed quick state reports a visible load error");
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-load-error.bmp");
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Check(save(5).machine == first.machine,
                  "Rejected state file preserves paused execution");
            Key(window, SDL_SCANCODE_Z, SDLK_Z, SDL_KMOD_CTRL);
            Check(save(6).machine == second.machine, "Rejected load preserves the previous undo");
            Key(window, SDL_SCANCODE_Y, SDLK_Y, SDL_KMOD_CTRL);
            Check(save(7).machine == first.machine,
                  "Undo and redo remain available after a failed load");
            Key(window, SDL_SCANCODE_F3, SDLK_F3);
            const auto manager = ReadPicture(window);
            const float left = float(manager.width - 700) / 2;
            const float top = float(manager.height - 570) / 2;
            const auto named = states.Path({false, "New state"});
            Click(window, left + 42, top + 393);
            Wait([&] { return fs::is_regular_file(named); }, "Manager Add writes a named state");
            Check(StateFile::Read(named).machine == first.machine,
                  "Manager capture preserves the exact paused checkpoint");
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-manager.bmp");
            const auto originalNamed = ReadFile(named, 140 * 1024 * 1024);
            Click(window, left + 47, top + 425);
            Click(window, left + 78, top + 329);
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Check(ReadFile(named, 140 * 1024 * 1024) == originalNamed,
                  "Cancelling manager replacement preserves the original file");
            Click(window, left + 78, top + 329);
            Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
            Wait([&] { return StateFile::Read(named).machine == second.machine; },
                 "Manager undo and confirmed save retain the exact pre-load machine");
            const auto namedBytes = ReadFile(named, 140 * 1024 * 1024);
            Click(window, left + 127, top + 425);
            Click(window, left + 28, top + 329);
            Click(window, left + 47, top + 425);
            Click(window, left + 160, top + 361);
            Key(window, SDL_SCANCODE_A, SDLK_A, SDL_KMOD_CTRL);
            SDL_Event text{};
            text.type = SDL_EVENT_TEXT_INPUT;
            text.text.windowID = window;
            text.text.text = "Renamed state";
            Push(text);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Click(window, left + 114, top + 393);
            const auto renamed = states.Path({false, "Renamed state"});
            Wait([&] { return fs::exists(renamed) && !fs::exists(named); },
                 "Manager Rename preserves the state under its new name");
            Check(ReadFile(renamed, 140 * 1024 * 1024) == namedBytes,
                  "Manager rename does not rewrite the state payload");
            Click(window, left + 137, top + 329);
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Check(fs::exists(renamed), "Cancelling removal preserves the selected state");
            Click(window, left + 137, top + 329);
            Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
            Wait([&] { return !fs::exists(renamed); },
                 "Confirmed manager removal deletes the selected file");
            Click(window, left + 160, top + 44);
            Key(window, SDL_SCANCODE_DOWN, SDLK_DOWN);
            Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-manager-quick.bmp");
            Click(window, left + 78, top + 329);
            Key(window, SDL_SCANCODE_RETURN, SDLK_RETURN);
            Wait(
                [&] {
                    try {
                        return StateFile::Read(states.Path(StateStorage::Quick(0))).machine ==
                               first.machine;
                    } catch (const std::exception&) {
                        return false;
                    }
                },
                "Manager quick category repairs only the selected quick slot");
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Check(save(8).machine == first.machine,
                  "Opening and closing the manager preserves paused execution");
            Key(window, SDL_SCANCODE_P, SDLK_P);
            Wait([&] { return ReadPicture(window).title.find("Paused") == std::string::npos; },
                 "Playback resumes after paused state operations");
            Key(window, SDL_SCANCODE_F3, SDLK_F3);
            Wait([&] { return ReadPicture(window).title.find("Paused") != std::string::npos; },
                 "Opening the manager suspends running playback");
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Wait([&] { return ReadPicture(window).title.find("Paused") == std::string::npos; },
                 "Closing the manager resumes the prior running mode");
            select(0);
            Key(window, SDL_SCANCODE_F4, SDLK_F4);
            Check(ReadPicture(window).title.find("Paused") == std::string::npos,
                  "Quick load retains running playback");
            Wait([&] { return ReadPicture(window).center != expectedPixel; },
                 "Running restore continues to present new fields");
            Key(window, SDL_SCANCODE_P, SDLK_P);
            const auto continued = save(9);
            Check(FrameCount(continued, rom) > FrameCount(first, rom) + 1,
                  "Running restore resets pacing and continues the restored machine timeline");
            Quit();
        } catch (...) {
            failure = std::current_exception();
            if (!returned && !pictures.empty())
                ReadPicture(window, pictures / "state-workflow-failure.bmp");
            if (!returned)
                Quit();
        }
    });
    const auto result = RunDesktop({}, {}, request, profile);
    returned = true;
    driver.join();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0, "State application exits normally");
    Check(PreferencesFile(profile).Load().stateSlot == 9,
          "Quick-slot selection survives application exit");
}

void TurboFields(const fs::path& directory) {
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    Check(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_VirtualJoystickDesc descriptor;
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.name = "Turbo application field clock";
    descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    const auto id = SDL_AttachVirtualJoystick(&descriptor);
    auto* joystick = SDL_OpenJoystick(id);
    Check(id != 0 && joystick, "Attach application turbo gamepad");

    struct Pad {
        SDL_JoystickID id;
        SDL_Joystick* joystick;

        ~Pad() {
            if (SDL_WasInit(SDL_INIT_JOYSTICK)) {
                SDL_CloseJoystick(joystick);
                SDL_DetachVirtualJoystick(id);
            }
        }
    } pad{id, joystick};

    Check(SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true),
          "Hold application turbo button");
    SDL_UpdateJoysticks();
    const auto profile = directory / "turbo.cfg", rom = directory / "turbo.sfc";
    PreferencesFile file(profile);
    auto preferences = file.Load();
    preferences.focus = FocusBehavior::AllowInput;
    preferences.input.turboFrequency = 2;
    preferences.input.players[0].buttons = {};
    preferences.input.players[0].turbo[4][0] = {InputBinding::Kind::GamepadButton,
                                                SDL_GAMEPAD_BUTTON_SOUTH};
    std::string deviceKey;
    for (const auto& device : SdlInputProvider::ConnectedGamepads())
        if (device.name == descriptor.name)
            deviceKey = device.key;
    Check(!deviceKey.empty(), "Select the application turbo gamepad");
    preferences.input.players[0].device = deviceKey;
    file.Save(preferences);
    auto image = Image();
    image[0x7fd6] = 2;
    image[0x7fd8] = 3;
    std::vector<uint8_t> code{0x78};
    const auto write = [&](uint16_t address, uint8_t value) {
        code.insert(code.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
    };
    const auto branch = [&](uint8_t opcode, size_t target) {
        code.push_back(opcode);
        const auto offset = int(target) - int(code.size() + 1);
        Check(offset >= -128 && offset <= 127, "Turbo program branch fits");
        code.push_back(uint8_t(offset));
    };
    write(0x2100, 0x80);
    write(0x2121, 0);
    write(0x2122, 0x1f);
    write(0x2122, 0);
    write(0x2100, 0x0f);
    write(0x4200, 1);
    code.insert(code.end(), {0xa2, 0x00, 0xa9, 0x00, 0x8f, 0x00, 0x00, 0x70});
    const auto waitStart = code.size();
    code.insert(code.end(), {0xad, 0x12, 0x42, 0x29, 0x01});
    branch(0xf0, waitStart);
    const auto waitEnd = code.size();
    code.insert(code.end(), {0xad, 0x12, 0x42, 0x29, 0x01});
    branch(0xd0, waitEnd);
    code.insert(code.end(), {0xad, 0x18, 0x42, 0x29, 0x80, 0x9f, 0x00, 0x01, 0x70, 0xe8, 0x8a, 0x8f,
                             0x00, 0x00, 0x70});
    branch(0x80, waitStart);
    std::copy(code.begin(), code.end(), image.begin());
    {
        std::ofstream out(rom, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
        Check(bool(out), "Application turbo program writes");
    }
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); },
                 "Turbo application creates a window");
            Check(!returned, "Turbo application stays open");
            Wait([&] { return ReadPicture(window).center == 0xff0000ff; },
                 "Turbo guest presents its initialized backdrop");
            Key(window, SDL_SCANCODE_P, SDLK_P);
            Wait([&] { return ReadPicture(window).title.find("Paused") != std::string::npos; },
                 "Pause the application turbo guest");
            for (unsigned field = 0; field < 24; ++field) {
                Key(window, SDL_SCANCODE_N, SDLK_N);
                if (field == 11)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            Quit();
        } catch (...) {
            failure = std::current_exception();
            if (!returned)
                Quit();
        }
    });
    const auto result = RunDesktop({}, {}, request, profile);
    returned = true;
    driver.join();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0, "Turbo application exits normally");
    auto battery = rom;
    battery.replace_extension(".srm");
    const auto bytes = ReadFile(battery, 8192);
    Check(bytes.size() == 8192 && bytes[0] >= 16,
          "Guest records enough automatic controller reads");
    const size_t first = size_t(bytes[0]) - 16;
    for (size_t row = first; row < size_t(bytes[0]); ++row) {
        const auto value = bytes[0x100 + row];
        Check(value == 0 || value == 0x80, "Guest records only the mapped turbo button");
        if (row + 2 < size_t(bytes[0]))
            Check(value != bytes[0x100 + row + 2],
                  "Desktop turbo holds each phase for two emulated fields");
        if (row + 4 < size_t(bytes[0]))
            Check(value == bytes[0x100 + row + 4],
                  "Paused host delay preserves the desktop turbo cycle");
    }
}

void CancelWorkflow(const fs::path& directory, const fs::path& pictures) {
    const auto rom = directory / "large-media.sfc";
    const auto data = directory / "large-media.msu";
    {
        const auto bytes = Image();
        std::ofstream other(directory / "state-game.sfc", std::ios::binary);
        other.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        std::ofstream file(rom, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        Check(bool(file), "Write cancellation cartridge");
        std::ofstream media(data, std::ios::binary);
        media.seekp(256 * 1024 * 1024 + 16);
        media.put(0);
        Check(bool(media), "Write sparse large-media fixture");
    }
    const auto touchMedia = [&] {
        std::fstream media(data, std::ios::binary | std::ios::in | std::ios::out);
        media.seekp(-1, std::ios::end);
        media.put(0);
        media.close();
        Check(bool(media), "Invalidate the media stamp while retaining identical contents");
    };
    const auto profile = directory / "cancel-preferences.cfg";
    PreferencesFile preferencesFile(profile);
    auto preferences = preferencesFile.Load();
    preferences.focus = FocusBehavior::AllowInput;
    preferences.hotkeys.gamepads[size_t(DesktopCommand::Pause)][0].controls[0] = {
        InputBinding::Kind::GamepadButton, SDL_GAMEPAD_BUTTON_SOUTH};
    preferencesFile.Save(preferences);
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage states(request);
    const auto path = states.Path(StateStorage::Quick(0));
    StateFile::Write(path, std::vector<uint8_t>(48, 0x71), {});
    const auto old = ReadFile(path, 1024);
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());
    SDL_VirtualJoystickDesc descriptor;
    SDL_INIT_INTERFACE(&descriptor);
    descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    descriptor.name = "State progress shortcut gamepad";
    descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    const auto id = SDL_AttachVirtualJoystick(&descriptor);
    auto* joystick = SDL_OpenJoystick(id);
    Check(id != 0 && joystick, "Attach progress shortcut gamepad");

    struct Pad {
        SDL_JoystickID id;
        SDL_Joystick* joystick;

        ~Pad() {
            if (SDL_WasInit(SDL_INIT_JOYSTICK)) {
                SDL_CloseJoystick(joystick);
                SDL_DetachVirtualJoystick(id);
            }
        }
    } pad{id, joystick};

    const auto button = [&](bool down) {
        struct Request {
            SDL_Joystick* joystick;
            bool down, success = false;
        } request{joystick, down};
        Check(SDL_RunOnMainThread(
                  [](void* userdata) {
                      auto& request = *static_cast<Request*>(userdata);
                      request.success = SDL_SetJoystickVirtualButton(
                          request.joystick, SDL_GAMEPAD_BUTTON_SOUTH, request.down);
                      SDL_UpdateJoysticks();
                  },
                  &request, true) &&
                  request.success,
              "Update progress shortcut on the main thread");
    };
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
    ErrorCapture errors;
    std::exception_ptr failure;
    std::vector<uint8_t> beforeQuit;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); },
                 "Cancellation application creates a window");
            Check(!returned, "Cancellation application stays open");
            Wait(
                [&] {
                    const auto color = ReadPicture(window).center;
                    return Profile(profile).recent.size() == 1 && color != 0xff000000 &&
                           color != 0xff1e1816;
                },
                "Cancellation guest completes a picture");
            Key(window, SDL_SCANCODE_P, SDLK_P);
            const auto start = [&](SDL_Scancode scan, SDL_Keycode key) {
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = window;
                event.key.scancode = scan;
                event.key.key = key;
                Push(event);
                event.type = SDL_EVENT_KEY_UP;
                Push(event);
                Wait([] { return ReadProgress().active; },
                     "Large-media state operation displays progress");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                Check(ReadProgress().active,
                      "Progress remains responsive during cold media checking");
            };
            const auto other = snes::core::PathText(directory / "state-game.sfc");
            const auto blockedActions = [&] {
                for (const auto scan : {SDL_SCANCODE_N, SDL_SCANCODE_F9}) {
                    SDL_Event event{};
                    event.type = SDL_EVENT_KEY_DOWN;
                    event.key.windowID = window;
                    event.key.scancode = scan;
                    Push(event);
                    event.type = SDL_EVENT_KEY_UP;
                    Push(event);
                }
                SDL_Event drop{};
                drop.type = SDL_EVENT_DROP_FILE;
                drop.drop.windowID = window;
                drop.drop.data = other.c_str();
                Push(drop);
            };
            start(SDL_SCANCODE_F2, SDLK_F2);
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-progress-media.bmp");
            blockedActions();
            auto progress = ReadProgress();
            Click(window, progress.x + 45, progress.y + progress.height - 22);
            Check(ReadFile(path, 1024) == old && !ReadProgress().active,
                  "Cancel button retains the previous quick state and closes progress");
            Key(window, SDL_SCANCODE_F2, SDLK_F2);
            const auto saved = StateFile::Read(path);
            for (bool load : {false, true}) {
                touchMedia();
                beforeQuit = ReadFile(path, 8 * 1024 * 1024);
                start(load ? SDL_SCANCODE_F4 : SDL_SCANCODE_F2, load ? SDLK_F4 : SDLK_F2);
                blockedActions();
                Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
                Check(ReadFile(path, 8 * 1024 * 1024) == beforeQuit &&
                          ReadPicture(window).title.find("Paused") != std::string::npos,
                      "Escape cancellation preserves the slot and pause setting");
                Key(window, SDL_SCANCODE_F2, SDLK_F2);
                Check(StateFile::Read(path).machine == saved.machine,
                      "Blocked frame, reset and drop actions cannot mutate the busy machine");
            }
            button(true);
            Wait([&] { return ReadPicture(window).title.find("Paused") == std::string::npos; },
                 "Gamepad shortcut resumes before a running capture");
            touchMedia();
            start(SDL_SCANCODE_F2, SDLK_F2);
            button(false);
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Check(ReadPicture(window).title.find("Paused") == std::string::npos,
                  "Cancelling a running capture retains running playback");
            button(true);
            Wait([&] { return ReadPicture(window).title.find("Paused") != std::string::npos; },
                 "Gamepad released during progress rearms on the next press");
            button(false);
            touchMedia();
            beforeQuit = ReadFile(path, 8 * 1024 * 1024);
            start(SDL_SCANCODE_F2, SDLK_F2);
            Quit();
        } catch (...) {
            failure = std::current_exception();
            if (!returned)
                Quit();
        }
    });
    const auto result = RunDesktop({}, {}, request, profile);
    returned = true;
    driver.join();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0 && errors.Empty(),
          "Cancellation and deferred quit complete without an error dialog");
    Check(ReadFile(path, 8 * 1024 * 1024) == beforeQuit,
          "Quit during capture unwinds before publishing a replacement file");
    const auto recent = PreferencesFile(profile).Load().recent;
    Check(recent.size() == 1 && recent[0].paths[0] == request.paths[0],
          "Busy file drops never replace the loaded session");
}

uint32_t PreviewPixel(SDL_WindowID window) {
    struct Request {
        SDL_WindowID window;
        uint32_t pixel = 0;
    } request{window};

    Check(SDL_RunOnMainThread(
              [](void* userdata) {
                  auto& request = *static_cast<Request*>(userdata);
                  const auto* manager = ImGui::GetCurrentContext()
                                            ? ImGui::FindWindowByName("State Manager")
                                            : nullptr;
                  auto* window = SDL_GetWindowFromID(request.window);
                  auto* renderer = window ? SDL_GetRenderer(window) : nullptr;
                  if (!manager || !manager->Active || !renderer)
                      return;
                  auto* surface = SDL_RenderReadPixels(renderer, nullptr);
                  if (!surface)
                      return;
                  const auto display = ImGui::GetIO().DisplaySize;
                  const int x =
                      int((manager->Pos.x + manager->Size.x - 120) * surface->w / display.x);
                  const int y = int((manager->Pos.y + 160) * surface->h / display.y);
                  Uint8 r = 0, g = 0, b = 0, a = 0;
                  if (SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a))
                      request.pixel = uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(g) << 8 | r;
                  SDL_DestroySurface(surface);
              },
              &request, true),
          "Preview pixels are read on the UI thread");
    return request.pixel;
}

std::string ReadUiText() {
    struct Request {
        std::atomic<bool> done{false};
        ImGuiID begin = 0, end = 0;
        std::string text;
    } request;

    Check(SDL_RunOnMainThread(
              [](void* userdata) {
                  auto& request = *static_cast<Request*>(userdata);
                  auto* context = ImGui::GetCurrentContext();
                  ImGuiContextHook begin;
                  begin.Type = ImGuiContextHookType_NewFramePost;
                  begin.Callback = [](ImGuiContext*, ImGuiContextHook*) { ImGui::LogToBuffer(); };
                  request.begin = ImGui::AddContextHook(context, &begin);
                  ImGuiContextHook end;
                  end.Type = ImGuiContextHookType_EndFramePre;
                  end.UserData = &request;
                  end.Callback = [](ImGuiContext* context, ImGuiContextHook* hook) {
                      auto& request = *static_cast<Request*>(hook->UserData);
                      request.text = context->LogBuffer.c_str();
                      ImGui::LogFinish();
                      ImGui::RemoveContextHook(context, request.begin);
                      ImGui::RemoveContextHook(context, request.end);
                      request.done = true;
                  };
                  request.end = ImGui::AddContextHook(context, &end);
              },
              &request, true),
          "UI text observer is installed on the owning thread");
    Wait([&] { return request.done.load(); }, "A complete UI frame reaches the text observer");
    return request.text;
}

void PreviewWorkflow(const fs::path& directory, const fs::path& pictures) {
    const auto rom = directory / "preview-game.sfc";
    {
        const auto image = Image();
        std::ofstream file(rom, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
        Check(bool(file), "Write preview workflow cartridge");
    }
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage storage(request);
    const auto large = storage.Path({false, "A large state"});
    const auto small = storage.Path({false, "B small state"});
    constexpr uint32_t largeColor = 0xff33aa11, smallColor = 0xffdd4411;
    {
        std::vector<uint8_t> machine(StateFile::maximumMachineBytes, 0x36);
        StateFile::Write(large, machine, {1, 1, {largeColor}});
    }
    StateFile::Write(small, std::vector<uint8_t>(48, 0x79), {1, 1, {smallColor}});
    const auto modified = fs::last_write_time(large);
    const auto profile = directory / "preview-preferences.cfg";
    PreferencesFile preferencesFile(profile);
    auto preferences = preferencesFile.Load();
    preferences.focus = FocusBehavior::AllowInput;
    preferencesFile.Save(preferences);
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(WindowCreated, &window), SDL_GetError());
    ErrorCapture errors;
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); },
                 "Preview application creates a window");
            Check(!returned, "Preview application stays open");
            Wait(
                [&] {
                    const auto color = ReadPicture(window).center;
                    return Profile(profile).recent.size() == 1 && color != 0xff000000 &&
                           color != 0xff1e1816;
                },
                "Preview guest presents a completed picture");
            Key(window, SDL_SCANCODE_P, SDLK_P);
            Key(window, SDL_SCANCODE_F2, SDLK_F2);
            const auto before = StateFile::Read(storage.Path(StateStorage::Quick(0))).machine;
            SDL_Event open{};
            open.type = SDL_EVENT_KEY_DOWN;
            open.key.windowID = window;
            open.key.scancode = SDL_SCANCODE_F3;
            open.key.key = SDLK_F3;
            Push(open);
            open.type = SDL_EVENT_KEY_UP;
            Push(open);
            Wait([] { return ReadProgress().active; },
                 "Large preview read displays cancellable progress");
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-preview-progress.bmp");
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Check(fs::last_write_time(large) == modified && PreviewPixel(window) != largeColor,
                  "Cancelled preview leaves its source intact and publishes no picture");
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Key(window, SDL_SCANCODE_F2, SDLK_F2);
            Check(StateFile::Read(storage.Path(StateStorage::Quick(0))).machine == before,
                  "Cancelled preview leaves the paused machine unchanged");
            Key(window, SDL_SCANCODE_F3, SDLK_F3);
            Wait([&] { return PreviewPixel(window) == largeColor; },
                 "Validated large preview reaches the renderer");
            const auto picture = ReadPicture(window);
            const float left = float(picture.width - 700) / 2,
                        top = float(picture.height - 570) / 2;
            Click(window, left + 70, top + 140);
            Wait([&] { return PreviewPixel(window) == smallColor; },
                 "Selection replaces the previous preview safely");
            Click(window, left + 70, top + 140);
            Check(ReadUiText().find("does not match") != std::string::npos,
                  "Successful preview reading preserves a rejected load's visible error");
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-preview-load-error.bmp");
            {
                std::fstream file(large, std::ios::in | std::ios::out | std::ios::binary);
                file.seekp(64 + 65537);
                file.put(0x51);
                file.close();
                Check(bool(file), "Change a machine byte outside the preview");
            }
            Click(window, left + 70, top + 90);
            Check(PreviewPixel(window) != largeColor && PreviewPixel(window) != smallColor,
                  "Checksum failure clears the old picture and never publishes unchecked pixels");
            Check(ReadUiText().find("checksum") != std::string::npos,
                  "Preview checksum errors remain visible inside the manager");
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-preview-error.bmp");
            {
                std::fstream file(large, std::ios::in | std::ios::out | std::ios::binary);
                file.seekp(64 + 65537);
                file.put(0x36);
                file.close();
                Check(bool(file), "Repair preview fixture");
            }
            Click(window, left + 70, top + 90);
            Wait([&] { return PreviewPixel(window) == largeColor; },
                 "Selecting a repaired state retries its preview");
            if (!pictures.empty())
                ReadPicture(window, pictures / "state-preview-ready.bmp");
            Key(window, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
            Key(window, SDL_SCANCODE_F2, SDLK_F2);
            Check(StateFile::Read(storage.Path(StateStorage::Quick(0))).machine == before,
                  "Preview reading, selection and recovery do not advance or restore execution");
            Quit();
        } catch (...) {
            failure = std::current_exception();
            if (!returned)
                Quit();
        }
    });
    const auto result = RunDesktop({}, {}, request, profile);
    returned = true;
    driver.join();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0 && errors.Empty(),
          "Preview cancellation and file errors remain inside the manager");
}

void CaptureFocus(const fs::path& directory, FocusBehavior behavior) {
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_GAMEPAD), SDL_GetError());

    struct Pad {
        SDL_Joystick* joystick = nullptr;
        SDL_JoystickID id = 0;
        std::atomic<SDL_WindowID> window{0};
        std::atomic<bool> held{false}, hide{false}, hidden{false};

        Pad() {
            SDL_VirtualJoystickDesc descriptor;
            SDL_INIT_INTERFACE(&descriptor);
            descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            descriptor.name = "Capture focus gamepad";
            descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
            descriptor.userdata = this;
            descriptor.Update = [](void* userdata) {
                auto& self = *static_cast<Pad*>(userdata);
                if (!self.joystick)
                    return;
                SDL_SetJoystickVirtualButton(self.joystick, SDL_GAMEPAD_BUTTON_SOUTH, self.held);
                SDL_SetJoystickVirtualButton(self.joystick, SDL_GAMEPAD_BUTTON_EAST, self.held);
                if (self.hide && ImGui::GetCurrentContext()) {
                    const auto* progress = ImGui::FindWindowByName("State operation");
                    if (progress && progress->Active)
                        if (auto* window = SDL_GetWindowFromID(self.window)) {
                            SDL_HideWindow(window);
                            self.hidden = true;
                            self.hide = false;
                        }
                }
            };
            id = SDL_AttachVirtualJoystick(&descriptor);
            joystick = SDL_OpenJoystick(id);
            Check(id != 0 && joystick, "Attach capture focus gamepad");
        }

        ~Pad() {
            if (SDL_WasInit(SDL_INIT_JOYSTICK)) {
                SDL_CloseJoystick(joystick);
                SDL_DetachVirtualJoystick(id);
            }
        }
    } pad;

    const auto rom = directory / ("focus-" + std::to_string(int(behavior)) + ".sfc");
    auto image = Image();
    std::vector<uint8_t> program{0x78};
    const auto write = [&](uint16_t address, uint8_t value) {
        program.insert(program.end(), {0xa9, value, 0x8d, uint8_t(address), uint8_t(address >> 8)});
    };
    write(0x2100, 0x80);
    write(0x2121, 0);
    write(0x2122, 0x1f);
    write(0x2122, 0);
    write(0x2100, 0x0f);
    write(0x4200, 1);
    program.insert(program.end(), {0xad, 0x19, 0x42, 0x29, 0x10, 0xf0, 0xf9});
    for (unsigned channel = 0; channel < 8; ++channel) {
        const uint16_t base = uint16_t(0x4300 + channel * 16);
        for (unsigned byte = 0; byte < 7; ++byte)
            write(uint16_t(base + byte), byte == 1 ? 0x22 : byte == 4 ? 0x7e : 0);
    }
    write(0x420b, 0xff);
    program.push_back(0xdb);
    std::copy(program.begin(), program.end(), image.begin());
    {
        std::ofstream file(rom, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
        Check(bool(file), "Write focus capture DMA program");
    }
    auto profile = rom;
    profile.replace_extension(".cfg");
    PreferencesFile file(profile);
    auto preferences = file.Load();
    preferences.focus = behavior;
    preferences.console.ports[1] = snes::core::ControllerDevice::SuperScope;
    preferences.input.players[0].buttons = {};
    preferences.input.players[0].buttons[4][0] = {InputBinding::Kind::GamepadButton,
                                                  SDL_GAMEPAD_BUTTON_SOUTH};
    preferences.input.players[0].buttons[10][0] = {InputBinding::Kind::GamepadButton,
                                                   SDL_GAMEPAD_BUTTON_EAST};
    file.Save(preferences);
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage storage(request);
    const auto saved = storage.Path(StateStorage::Quick(0));
    Check(SDL_AddEventWatch(WindowCreated, &pad.window), SDL_GetError());
    std::atomic<bool> returned{false};
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return pad.window.load() || returned.load(); },
                 "Focus application creates a window");
            Check(!returned, "Focus application stays open");
            Wait(
                [&] {
                    const auto color = ReadPicture(pad.window).center;
                    return color == 0xff0000ff || color == 0xff00ff00;
                },
                "DMA guest waits for its start button");
            Key(pad.window, SDL_SCANCODE_P, SDLK_P);
            Check(SDL_RunOnMainThread(
                      [](void* userdata) {
                          auto* window = SDL_GetWindowFromID(static_cast<Pad*>(userdata)->window);
                          int width = 0, height = 0;
                          SDL_GetWindowSize(window, &width, &height);
                          SDL_WarpMouseInWindow(window, width / 2.0f, height / 2.0f);
                      },
                      &pad, true),
                  "Place pointer aim inside the game picture before capture");
            pad.held = true;
            Key(pad.window, SDL_SCANCODE_N, SDLK_N);
            Key(pad.window, SDL_SCANCODE_N, SDLK_N);
            pad.hide = true;
            Key(pad.window, SDL_SCANCODE_F2, SDLK_F2);
            Check(pad.hidden && fs::exists(saved),
                  "Focus changes while capture is servicing progress");
            Check(SDL_RunOnMainThread(
                      [](void* userdata) {
                          auto* window = SDL_GetWindowFromID(static_cast<Pad*>(userdata)->window);
                          if (window)
                              SDL_ShowWindow(window);
                      },
                      &pad, true),
                  "Restore the hidden test window");
            const auto state = StateFile::Read(saved);
            snes::core::Emulator machine;
            std::string error;
            Check(machine.LoadCartridgeFromFile(request.paths[0], &error),
                  "Open focus continuation target");
            machine.RestoreState(state.machine);
            Check(machine.CurrentFrame() > 8, "Capture drains the long DMA across display fields");
            auto& ports = machine.GetAutoJoypad().Ports();
            ports.SetGunCallback({});
            ports.SetLatch(true);
            ports.SetLatch(false);
            uint8_t offscreen = 0;
            for (unsigned bit = 0; bit <= 6; ++bit)
                offscreen = ports.Read(1);
            Check((offscreen & 1) != 0, "Pointer aim captured after focus loss remains offscreen");
            const auto next = machine.StepFrame();
            Check(next.latchedInput.a == (behavior == FocusBehavior::AllowInput),
                  "Capture applies the selected focus policy to fields sampled after focus loss");
            Quit();
        } catch (...) {
            failure = std::current_exception();
            if (!returned)
                Quit();
        }
    });
    const auto result = RunDesktop({}, {}, request, profile);
    returned = true;
    driver.join();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0, "Focus capture application exits normally");
}
} // namespace

int main(int argc, char** argv) {
    const auto directory =
        fs::temp_directory_path() /
        ("cupid-state-app-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(directory))
        return 2;

    struct Cleanup {
        fs::path path;

        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
            SDL_Quit();
        }
    } cleanup{directory};

    try {
        const bool cancelOnly = argc > 1 && std::string_view(argv[1]) == "--cancel-only";
        const bool previewOnly = argc > 1 && std::string_view(argv[1]) == "--preview-only";
        const bool focusOnly = argc > 1 && std::string_view(argv[1]) == "--focus-only";
        const int pictureIndex = cancelOnly || previewOnly || focusOnly ? 2 : 1;
        const auto pictures =
            argc > pictureIndex ? snes::core::FilePath(argv[pictureIndex]) : fs::path{};
        if (!pictures.empty())
            fs::create_directories(pictures);
        if (!cancelOnly && !previewOnly && !focusOnly) {
            Workflow(directory, pictures);
            TurboFields(directory);
        }
        if (!previewOnly && !focusOnly)
            CancelWorkflow(directory, pictures);
        if (!cancelOnly && !focusOnly)
            PreviewWorkflow(directory, pictures);
        if (!cancelOnly && !previewOnly)
            for (auto behavior :
                 {FocusBehavior::Pause, FocusBehavior::BlockInput, FocusBehavior::AllowInput})
                CaptureFocus(directory, behavior);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
