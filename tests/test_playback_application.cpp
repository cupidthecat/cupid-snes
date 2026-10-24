#include "ConsoleConfig.hpp"
#include "cartridge/StateFixture.hpp"
#include "desktop/ApplicationDriver.hpp"
#include "desktop/DesktopApplication.hpp"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/InputMovie.hpp"
#include "storage/StateStorage.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>

using namespace snes::tests::desktop;
using namespace snes::frontend;
namespace fs = std::filesystem;

namespace {
struct View {
    std::string title;
    bool movies = false, error = false, focused = false;
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
        if (auto* window = ImGui::FindWindowByName("Input Movies")) {
            result.movies = window->Active;
            result.focused = ImGui::GetCurrentContext()->NavId == window->GetID(control);
        }
        if (!picture.empty()) {
            auto* surface = SDL_RenderReadPixels(SDL_GetRenderer(host), nullptr);
            Check(surface, "Movie manager renders a picture");
            Check(SDL_SaveBMP(surface, snes::core::PathText(picture).c_str()),
                  "Save movie manager picture");
            SDL_DestroySurface(surface);
        }
    });
    return result;
}

void Activate(SDL_WindowID window, const char* control) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto view = Observe(window, control);
        Check(view.movies, "Movie manager remains open during navigation");
        if (view.focused) {
            Key(window, SDL_SCANCODE_RETURN);
            return;
        }
        Key(window, SDL_SCANCODE_TAB);
    }
    std::fprintf(stderr, "Movie control not focused: %s\n", control);
    throw std::runtime_error("Movie manager controls accept keyboard navigation");
}

void Hold(SDL_WindowID window, SDL_Scancode scan, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = window;
    event.key.scancode = scan;
    event.key.key = SDL_GetKeyFromScancode(scan, 0, false);
    Push(event);
    Frames();
}

std::vector<fs::path> Movies(const fs::path& directory) {
    std::vector<fs::path> result;
    if (fs::is_directory(directory))
        for (const auto& file : fs::directory_iterator(directory))
            if (file.is_regular_file() && file.path().extension() == ".cmv")
                result.push_back(file.path());
    std::sort(result.begin(), result.end());
    return result;
}

void Workflow(const fs::path& directory, const fs::path& pictures) {
    auto image = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    const std::array<uint8_t, 16> code{0xa9, 1, 0x8d, 0,    0x42, 0xad, 0x16, 0x40,
                                       0x8f, 0, 0,    0x70, 0x4c, 5,    0x80, 0xea};
    std::copy(code.begin(), code.end(), image.begin());
    const auto rom = directory / "movie-game.sfc", profile = directory / "settings.cfg";
    {
        std::ofstream file(rom, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), image.size());
    }
    PreferencesFile file(profile);
    auto preferences = file.Load();
    preferences.focus = FocusBehavior::AllowInput;
    preferences.rewind = {1, 12, size_t(64) * 1024 * 1024};
    preferences.stateDirectory = snes::core::PathText(directory / "states");
    preferences.hotkeys.bindings[size_t(DesktopCommand::MovieManager)][0] = {SDL_SCANCODE_F9, 0};
    preferences.hotkeys.bindings[size_t(DesktopCommand::RecordMovie)][0] = {SDL_SCANCODE_F10, 0};
    file.Save(preferences);
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage states(request, directory / "states");
    const auto movies = states.Directory() / "movies",
               backup = states.Directory() / "movies-backup";
    const auto quick = states.Path(StateStorage::Quick(0));
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(Created, &window), "Observe playback application");
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); },
                 "Application creates its window");
            Check(!returned, "Application remains open");
            auto validator =
                std::make_unique<snes::core::Emulator>(snes::tests::DeterministicConsoleConfig());
            Check(validator->LoadCartridge(image), "Load independent checkpoint validator");
            const auto position = [&] {
                return validator->PrepareStateRestore(StateFile::Read(quick).machine).Position();
            };
            for (unsigned attempt = 0; attempt < 30; ++attempt) {
                Key(window, SDL_SCANCODE_F2);
                if (position().frame >= 8)
                    break;
            }
            Key(window, SDL_SCANCODE_P);
            Wait([&] { return Observe(window).title.find("Paused") != std::string::npos; },
                 "Pause playback");
            Key(window, SDL_SCANCODE_F2);
            const auto before = position();
            Key(window, SDL_SCANCODE_BACKSPACE, SDL_KMOD_SHIFT);
            Key(window, SDL_SCANCODE_F2);
            Check(position().masterCycles < before.masterCycles,
                  "Rewind restores an earlier complete field");
            const auto firstRewind = position();
            Hold(window, SDL_SCANCODE_BACKSPACE, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            Hold(window, SDL_SCANCODE_BACKSPACE, false);
            Key(window, SDL_SCANCODE_F2);
            Check(position().masterCycles <= firstRewind.masterCycles,
                  "Held rewind moves backward while paused and stops on release");
            Key(window, SDL_SCANCODE_F10);
            Wait([&] { return Observe(window).title.find("[Recording]") != std::string::npos; },
                 "Begin movie recording");
            for (unsigned frame = 0; frame < 3; ++frame)
                Key(window, SDL_SCANCODE_N);
            Key(window, SDL_SCANCODE_S, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            auto saved = Movies(movies);
            Check(saved.size() == 1, "Stop writes one complete movie");
            const auto movie = InputMovie::Read(saved[0]);
            Check(movie.Info().lastFrame >= movie.Info().firstFrame + 3 && !movie.Inputs()->empty(),
                  "Desktop recording retains frame advances and actual input queries");
            Key(window, SDL_SCANCODE_F2);
            const auto prior = StateFile::Read(quick).machine;
            Key(window, SDL_SCANCODE_F9);
            Wait([&] { return Observe(window).movies; }, "Open saved movie manager");
            if (!pictures.empty())
                Observe(window, "", pictures / "input-movies.bmp");
            Activate(window, "Play Selected");
            Activate(window, "Close");
            Wait([&] { return !Observe(window).movies; },
                 "Close movie manager without leaking shortcuts");
            for (uint64_t frame = movie.Info().firstFrame; frame <= movie.Info().lastFrame; ++frame)
                Key(window, SDL_SCANCODE_N);
            Check(!Observe(window).error, "Recorded input reaches the endpoint without mismatch");
            Key(window, SDL_SCANCODE_S, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Key(window, SDL_SCANCODE_Z, SDL_KMOD_CTRL);
            Key(window, SDL_SCANCODE_F2);
            Check(StateFile::Read(quick).machine == prior,
                  "Undo restores the machine from before movie playback");
            Key(window, SDL_SCANCODE_P);
            Hold(window, SDL_SCANCODE_TAB, true);
            Wait([&] { return Observe(window).title.find("400% speed") != std::string::npos; },
                 "Held fast forward changes host speed");
            Hold(window, SDL_SCANCODE_TAB, false);
            Wait([&] { return Observe(window).title.find("400% speed") == std::string::npos; },
                 "Releasing fast forward restores normal speed");
            Key(window, SDL_SCANCODE_TAB, SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).title.find("400% speed") != std::string::npos; },
                 "Fast forward toggle persists");
            Hold(window, SDL_SCANCODE_GRAVE, true);
            Wait([&] { return Observe(window).title.find("50% speed") != std::string::npos; },
                 "Held slow motion overrides the speed toggle");
            Hold(window, SDL_SCANCODE_GRAVE, false);
            Key(window, SDL_SCANCODE_0, SDL_KMOD_CTRL);
            Wait([&] { return Observe(window).title.find("% speed") == std::string::npos; },
                 "Normal speed clears speed toggles");
            Key(window, SDL_SCANCODE_P);
            Key(window, SDL_SCANCODE_F10);
            Key(window, SDL_SCANCODE_N);
            fs::rename(movies, backup);
            {
                std::ofstream blocked(movies);
                blocked << "blocked destination";
            }
            Key(window, SDL_SCANCODE_S, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).error; },
                 "A failed movie write opens a recoverable error");
            Check(Observe(window).title.find("Recording not saved") != std::string::npos,
                  "Failed writes keep the completed recording visible");
            fs::remove(movies);
            fs::rename(backup, movies);
            Key(window, SDL_SCANCODE_ESCAPE);
            Key(window, SDL_SCANCODE_S, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Check(Movies(movies).size() == 2 && InputMovie::Read(saved[0]).Info() == movie.Info(),
                  "Retry saves the retained recording without changing earlier movies");
            Key(window, SDL_SCANCODE_F10);
            Key(window, SDL_SCANCODE_N);
            Key(window, SDL_SCANCODE_Q, SDL_KMOD_CTRL, 0, false);
        } catch (...) {
            failure = std::current_exception();
            std::error_code ignored;
            if (fs::is_regular_file(movies, ignored))
                fs::remove(movies, ignored);
            if (fs::exists(backup, ignored))
                fs::rename(backup, movies, ignored);
            if (!returned)
                Quit();
        }
    });
    const auto result = RunDesktop(snes::tests::DeterministicConsoleConfig(), {}, request, profile);
    returned = true;
    driver.join();
    SDL_RemoveEventWatch(Created, &window);
    SDL_Quit();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0 && Movies(movies).size() == 3,
          "Quit saves an active recording before releasing its machine");
}
} // namespace

int main(int argc, char** argv) {
    const auto directory =
        fs::temp_directory_path() /
        ("cupid-playback-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(directory);
        fs::path pictures;
        if (argc > 1) {
            pictures = snes::core::FilePath(argv[1]);
            fs::create_directories(pictures);
        }
        Workflow(directory, pictures);
        Check(fs::weakly_canonical(directory).parent_path() ==
                  fs::weakly_canonical(fs::temp_directory_path()),
              "Test cleanup stays inside its temporary root");
        fs::remove_all(directory);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Playback application: %s\n", error.what());
        return 1;
    }
    std::puts("Playback application checks passed");
    return 0;
}
