#include "ConsoleConfig.hpp"
#include "cartridge/StateFixture.hpp"
#include "cheats/CheatBook.hpp"
#include "desktop/DesktopApplication.hpp"
#include "imgui_internal.h"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/StateStorage.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace snes::frontend;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

template <class Predicate> void Wait(Predicate predicate, const char* message) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < end, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Push(SDL_Event event) {
    Check(SDL_PushEvent(&event), "Queue application input");
}

void Key(SDL_WindowID window, SDL_Scancode scan, SDL_Keymod modifiers = 0) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = window;
    event.key.scancode = scan;
    event.key.key = SDL_GetKeyFromScancode(scan, modifiers, false);
    event.key.mod = modifiers;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    event.type = SDL_EVENT_KEY_UP;
    event.key.mod = 0;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
}

struct View {
    bool active = false, paused = false;
    float applyX = 0, applyY = 0, groupX = 0, groupY = 0, addX = 0, addY = 0, codesX = 0,
          codesY = 0;
    std::string picture;
};

View Observe(SDL_WindowID window, const fs::path& picture = {}) {
    struct Query {
        SDL_WindowID window;
        View view;
    } query{window, {}};

    query.view.picture = snes::core::PathText(picture);
    Check(SDL_RunOnMainThread(
              [](void* data) {
                  auto& query = *static_cast<Query*>(data);
                  auto& view = query.view;
                  auto* window = SDL_GetWindowFromID(query.window);
                  view.paused =
                      window && std::string_view(SDL_GetWindowTitle(window)).find("Paused") !=
                                    std::string_view::npos;
                  if (!ImGui::GetCurrentContext())
                      return;
                  if (const auto* editor = ImGui::FindWindowByName("Cheats")) {
                      view.active = editor->Active;
                      view.applyX = editor->Pos.x + 28;
                      view.applyY = editor->DC.CursorPosPrevLine.y + 12;
                      for (const auto* child : ImGui::GetCurrentContext()->Windows)
                          if (child->ParentWindow == editor &&
                              std::string_view(child->Name).find("Cheat list") !=
                                  std::string_view::npos) {
                              view.groupX = child->WorkRect.Min.x + 8;
                              view.groupY = child->WorkRect.Min.y + 8;
                              view.addX = editor->Pos.x + 28;
                              view.addY = child->Pos.y + child->Size.y + 20;
                          }
                      for (const auto* child : ImGui::GetCurrentContext()->Windows)
                          if (child->ParentWindow == editor &&
                              std::string_view(child->Name).find("##codes") !=
                                  std::string_view::npos) {
                              view.codesX = child->WorkRect.Min.x + 20;
                              view.codesY = child->WorkRect.Min.y + 10;
                          }
                  }
                  if (!view.picture.empty()) {
                      auto* renderer = window ? SDL_GetRenderer(window) : nullptr;
                      auto* surface = renderer ? SDL_RenderReadPixels(renderer, nullptr) : nullptr;
                      if (surface) {
                          SDL_SaveBMP(surface, view.picture.c_str());
                          SDL_DestroySurface(surface);
                      }
                  }
              },
              &query, true),
          "Observe the owning application thread");
    return query.view;
}

void Click(SDL_WindowID window, float x, float y) {
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
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = false;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void Quit() {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    Push(event);
}

void Text(SDL_WindowID window, const char* value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = window;
    event.text.text = value;
    Push(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

bool SDLCALL Created(void* data, SDL_Event* event) {
    if (event->type == SDL_EVENT_WINDOW_SHOWN)
        static_cast<std::atomic<SDL_WindowID>*>(data)->store(event->window.windowID);
    return true;
}

void Workflow(const fs::path& directory, const fs::path& pictures) {
    const auto rom = directory / "game.sfc", profile = directory / "settings.cfg";
    auto image = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    image[0] = 0xdb;
    {
        std::ofstream out(rom, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), image.size());
    }
    PreferencesFile preferences(profile);
    auto settings = preferences.Load();
    settings.focus = FocusBehavior::AllowInput;
    preferences.Save(settings);
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage states(request);
    const auto cheatPath = states.Directory() / "cheats.ccht";
    CheatBook book{true, {{"Health ## label", "7E1234=55", false}}};
    CheatStorage::Write(cheatPath, book, snes::core::CheatSystem::Console);
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(Created, &window), "Install window observer");
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); }, "Create application window");
            Check(!returned, "Application remains available");
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            Key(window, SDL_SCANCODE_C, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait(
                [&] {
                    const auto view = Observe(window);
                    return view.active && view.paused && view.groupX != 0;
                },
                "Cheat shortcut opens the editor and pauses gameplay");
            if (!pictures.empty())
                Observe(window, pictures / "cheat-editor.bmp");
            const auto view = Observe(window);
            Click(window, view.groupX, view.groupY);
            Click(window, view.applyX, view.applyY);
            Wait(
                [&] {
                    return CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console)
                        .entries[0]
                        .enabled;
                },
                "The rendered checkbox and Apply save the enabled cheat");
            auto edit = Observe(window);
            Check(edit.codesX != 0, "Multiline code editor is rendered");
            Click(window, edit.codesX, edit.codesY);
            Key(window, SDL_SCANCODE_A, SDL_KMOD_CTRL);
            Text(window, "invalid");
            edit = Observe(window);
            Click(window, edit.applyX, edit.applyY);
            Check(
                CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console).entries[0].codes ==
                    "7E1234=55",
                "Invalid editor input preserves the previously saved cheat");
            edit = Observe(window);
            Click(window, edit.codesX, edit.codesY);
            Key(window, SDL_SCANCODE_A, SDL_KMOD_CTRL);
            Text(window, "7E1234=66");
            edit = Observe(window);
            Click(window, edit.applyX, edit.applyY);
            Wait(
                [&] {
                    return CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console)
                               .entries[0]
                               .codes == "7E1234=66";
                },
                "Correcting an invalid code retries the same editor changes");
            edit = Observe(window);
            Click(window, edit.addX, edit.addY);
            edit = Observe(window);
            Click(window, edit.codesX, edit.codesY);
            Text(window, "7E1235=77");
            edit = Observe(window);
            Click(window, edit.applyX, edit.applyY);
            Wait(
                [&] {
                    return CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console)
                               .entries.size() == 2;
                },
                "Add creates a separately persisted disabled group");
            edit = Observe(window);
            Click(window, edit.addX + 52, edit.addY);
            edit = Observe(window);
            Click(window, edit.applyX, edit.applyY);
            Wait(
                [&] {
                    return CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console)
                               .entries.size() == 1;
                },
                "Remove deletes the selected group on Apply");
            Key(window, SDL_SCANCODE_F2);
            Check(!fs::exists(states.Path(StateStorage::Quick(0))),
                  "State shortcuts are blocked while editing cheats");
            Key(window, SDL_SCANCODE_ESCAPE);
            Wait(
                [&] {
                    const auto view = Observe(window);
                    return !view.active && !view.paused;
                },
                "Closing restores prior running playback");
            Key(window, SDL_SCANCODE_F8);
            Wait(
                [&] {
                    return !CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console).enabled;
                },
                "Global shortcut persists the disabled flag");
            Key(window, SDL_SCANCODE_F8);
            Key(window, SDL_SCANCODE_P);
            Wait([&] { return Observe(window).paused; },
                 "Pause before observing the applied machine");
            Key(window, SDL_SCANCODE_F2);
            const auto quick = states.Path(StateStorage::Quick(0));
            Wait([&] { return fs::is_regular_file(quick); },
                 "Actual application saves the cheated machine");
            const auto state = StateFile::Read(quick);
            auto witness =
                std::make_unique<snes::core::Emulator>(snes::tests::DeterministicConsoleConfig());
            Check(witness->LoadCartridge(image), "Load independent witness");
            witness->RestoreState(state.machine);
            Check(witness->GetBus().WramData()[0x1234] == 0x66,
                  "A saved machine proves the desktop cheat reached live WRAM");
            Key(window, SDL_SCANCODE_C, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).active; }, "Reopen while paused");
            const auto again = Observe(window);
            Click(window, again.groupX, again.groupY);
            Key(window, SDL_SCANCODE_ESCAPE);
            Check(Observe(window).paused &&
                      CheatStorage::Read(cheatPath, snes::core::CheatSystem::Console)
                          .entries[0]
                          .enabled,
                  "Close discards unapplied edits and preserves the prior pause state");
            Quit();
        } catch (...) {
            failure = std::current_exception();
            if (!returned)
                Quit();
        }
    });
    const auto result = RunDesktop(snes::tests::DeterministicConsoleConfig(), {}, request, profile);
    returned = true;
    driver.join();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0, "Application exits normally");
}
} // namespace

int main(int argc, char** argv) {
    const auto directory =
        fs::temp_directory_path() /
        ("cupid-cheat-app-" +
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
        const auto pictures = argc > 1 ? snes::core::FilePath(argv[1]) : fs::path{};
        if (!pictures.empty())
            fs::create_directories(pictures);
        Workflow(directory, pictures);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
