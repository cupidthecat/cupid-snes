#include "ConsoleConfig.hpp"
#include "archive/ArchiveFixture.hpp"
#include "cartridge/StateFixture.hpp"
#include "desktop/ApplicationDriver.hpp"
#include "desktop/DesktopApplication.hpp"
#include "session/GameAssets.hpp"
#include "settings/Preferences.hpp"
#include "storage/AtomicFile.hpp"
#include "storage/StateStorage.hpp"
#include <algorithm>
#include <cstdio>

using namespace snes::tests::desktop;
using namespace snes::tests::archives;
using namespace snes::frontend;
namespace fs = std::filesystem;

namespace {
struct View {
    std::string title;
    bool chooser = false, error = false, focused = false, editing = false;
};

View Observe(SDL_WindowID id, const char* control = "", const fs::path& picture = {}) {
    View result;
    OnMain([&] {
        auto* host = SDL_GetWindowFromID(id);
        if (host)
            result.title = SDL_GetWindowTitle(host);
        if (!ImGui::GetCurrentContext())
            return;
        if (const auto* error = ImGui::FindWindowByName("Cupid-SNES error"))
            result.error = error->Active;
        if (auto* window = ImGui::FindWindowByName("Choose ROM in Archive")) {
            result.chooser = window->Active;
            result.focused = ImGui::GetCurrentContext()->NavId == window->GetID(control);
            result.editing = ImGui::GetCurrentContext()->ActiveId == window->GetID(control);
        }
        if (!picture.empty()) {
            auto* surface = SDL_RenderReadPixels(SDL_GetRenderer(host), nullptr);
            Check(surface, "Archive chooser renders a picture");
            Check(SDL_SaveBMP(surface, snes::core::PathText(picture).c_str()),
                  "Save chooser picture");
            SDL_DestroySurface(surface);
        }
    });
    return result;
}

void Activate(SDL_WindowID window, const char* control) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto view = Observe(window, control);
        Check(view.chooser, "Archive chooser stays open during keyboard navigation");
        if (view.focused) {
            Key(window, SDL_SCANCODE_RETURN);
            return;
        }
        Key(window, SDL_SCANCODE_TAB);
    }
    OnMain([&] {
        const auto& g = *ImGui::GetCurrentContext();
        std::fprintf(stderr, "Control %s nav=%08X active=%08X window=%s target=%08X\n", control,
                     g.NavId, g.ActiveId, g.NavWindow ? g.NavWindow->Name : "",
                     ImGui::FindWindowByName("Choose ROM in Archive")->GetID(control));
    });
    throw std::runtime_error(std::string("Archive control accepts keyboard navigation: ") +
                             control);
}

void Drop(SDL_WindowID window, const fs::path& path) {
    const auto text = snes::core::PathText(path);
    SDL_Event event{};
    event.type = SDL_EVENT_DROP_FILE;
    event.drop.windowID = window;
    event.drop.data = text.c_str();
    Push(event);
    Frames();
    Ready();
}

std::vector<uint8_t> Image(uint8_t value) {
    auto bytes = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    const std::array<uint8_t, 7> code{0xa9, value, 0x8f, 0, 0, 0x70, 0xdb};
    std::copy(code.begin(), code.end(), bytes.begin());
    return bytes;
}

void Workflow(const fs::path& home, const fs::path& picture) {
    Archive(home / "single.zip", "zip", {{"solo.sfc", Image(0x2a)}});
    Archive(home / "collection.7z", "7z",
            {{"first/game.sfc", Image(0x31)}, {"second/game.sfc", Image(0xb5)}});
    Write(home / "broken.rar", std::array<uint8_t, 4>{0, 1, 2, 3});
    PreferencesFile profile(home / "settings.cfg");
    auto preferences = profile.Load();
    preferences.focus = FocusBehavior::AllowInput;
    preferences.stateDirectory = snes::core::PathText(home / "states");
    preferences.hotkeys.bindings[size_t(DesktopCommand::Recent0)][0] = {SDL_SCANCODE_F10, 0};
    preferences.hotkeys.bindings[size_t(DesktopCommand::Unload)][0] = {SDL_SCANCODE_F9, 0};
    profile.Save(preferences);
    LaunchRequest initial;
    initial.paths[0] = snes::core::PathText(home / "single.zip");
    auto selected = initial;
    selected.archiveMembers[0] = "solo.sfc";
    const auto quick = StateStorage(selected, home / "states").Path(StateStorage::Quick(0));
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(Created, &window), "Observe archive application");
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); }, "Application creates window");
            Check(!returned, "Application remains open");
            Wait([&] { return Observe(window).title.find("solo.sfc") != std::string::npos; },
                 "A single archived ROM launches from the command line");
            Key(window, SDL_SCANCODE_P);
            Key(window, SDL_SCANCODE_F2);
            const auto prior = StateFile::Read(quick).machine;
            Drop(window, home / "collection.7z");
            Wait([&] { return Observe(window).chooser; }, "A dropped collection opens its chooser");
            Key(window, SDL_SCANCODE_ESCAPE);
            Wait([&] { return !Observe(window).chooser; }, "Cancel closes chooser");
            Key(window, SDL_SCANCODE_F2);
            Check(StateFile::Read(quick).machine == prior,
                  "Cancelling selection preserves the complete paused machine");
            Drop(window, home / "collection.7z");
            Wait([&] { return Observe(window).chooser; }, "Reopen archive chooser");
            Wait([&] { return Observe(window, "Find ROM").editing; },
                 "Search receives initial focus");
            Key(window, SDL_SCANCODE_A, SDL_KMOD_CTRL);
            Text(window, "second");
            Key(window, SDL_SCANCODE_RETURN);
            float x = 0, y = 0;
            OnMain([&] {
                for (const auto* child : ImGui::GetCurrentContext()->Windows)
                    if (child->Active && std::string_view(child->Name).find("/Archive ROMs_") !=
                                             std::string_view::npos) {
                        x = child->InnerRect.Min.x + 25;
                        y = child->InnerRect.Min.y + ImGui::GetStyle().WindowPadding.y +
                            ImGui::GetTextLineHeight() / 2;
                    }
                Check(x > 0 && y > 0, "Filtered ROM list is visible");
                SDL_WarpMouseInWindow(SDL_GetWindowFromID(window), x, y);
            });
            Click(window, x, y);
            if (!picture.empty())
                Observe(window, "", picture);
            Activate(window, "Open Selected");
            Wait([&] { return Observe(window).title.find("second/game.sfc") != std::string::npos; },
                 "The chosen ROM replaces the game");
            selected = profile.Load().recent.at(0);
            Check(selected.archiveMembers[0] == "second/game.sfc",
                  "Recent games persist the selected archive member");
            Key(window, SDL_SCANCODE_P);
            const auto secondQuick =
                StateStorage(selected, home / "states").Path(StateStorage::Quick(0));
            Key(window, SDL_SCANCODE_F2);
            const auto second = StateFile::Read(secondQuick).machine;
            Drop(window, home / "broken.rar");
            Wait([&] { return Observe(window).error; }, "Invalid archives show an error");
            Key(window, SDL_SCANCODE_ESCAPE);
            Key(window, SDL_SCANCODE_F2);
            Check(StateFile::Read(secondQuick).machine == second,
                  "Archive errors preserve the running game");
            Key(window, SDL_SCANCODE_F9);
            Wait([&] { return Observe(window).title == "Cupid-SNES"; }, "Unload archived game");
            auto save = GameAssets::SaveBase(selected, 0);
            save.replace_extension(".srm");
            Check(ReadFile(save, 8192).at(0) == 0xb5, "Selection uses its own battery file");
            Key(window, SDL_SCANCODE_F10);
            Wait([&] { return Observe(window).title.find("second/game.sfc") != std::string::npos; },
                 "Recent games reopen the exact selection without prompting");
            Check(!Observe(window).chooser, "Remembered ROM bypasses the chooser");
            Drop(window, home / "collection.7z");
            Wait([&] { return Observe(window).chooser; }, "Open chooser before quitting");
            Quit();
        } catch (...) {
            failure = std::current_exception();
            if (!returned)
                Quit();
        }
    });
    const auto result =
        RunDesktop(snes::tests::DeterministicConsoleConfig(), {}, initial, profile.Path());
    returned = true;
    driver.join();
    SDL_RemoveEventWatch(Created, &window);
    SDL_Quit();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0, "Quit remains available while choosing a ROM");
}
} // namespace

int main(int argc, char** argv) {
    const auto home =
        fs::temp_directory_path() / ("cupid-archive-ui-" + std::to_string(SDL_GetTicksNS()));
    try {
        fs::create_directories(home);
        Workflow(home, argc > 1 ? snes::core::FilePath(argv[1]) : fs::path{});
        Check(fs::weakly_canonical(home).parent_path() ==
                  fs::weakly_canonical(fs::temp_directory_path()),
              "Archive UI test cleanup stays inside its temporary root");
        fs::remove_all(home);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Archive application: %s\n", error.what());
        return 1;
    }
    std::puts("Archive application checks passed");
    return 0;
}
