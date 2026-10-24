#include "ConsoleConfig.hpp"
#include "cartridge/StateFixture.hpp"
#include "desktop/ApplicationDriver.hpp"
#include "desktop/DesktopApplication.hpp"
#include "imgui_internal.h"
#include "settings/Preferences.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/StateStorage.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace snes::tests::desktop;
using namespace snes::frontend;
namespace fs = std::filesystem;

namespace {
struct View {
    bool active = false, paused = false, focused = false, editing = false;
    float tabX = 0, tabY = 0;
    std::string selectedTab;
};

View Observe(SDL_WindowID id, const char* control = "", const char* tab = "",
             const fs::path& picture = {}) {
    View result;
    OnMain([&] {
        auto* host = SDL_GetWindowFromID(id);
        result.paused = host && std::string_view(SDL_GetWindowTitle(host)).find("Paused") !=
                                    std::string_view::npos;
        if (!ImGui::GetCurrentContext())
            return;
        auto& context = *ImGui::GetCurrentContext();
        auto* window = ImGui::FindWindowByName("Graphics and audio");
        result.active = window && window->Active;
        if (!result.active)
            return;
        const auto root = window->GetID(control);
        result.focused = context.NavId == root;
        result.editing = context.ActiveId == root;
        for (auto* child : context.Windows)
            if (child->ParentWindow == window &&
                std::string_view(child->Name).find("Hardware content") != std::string_view::npos)
                if (auto* tabs = context.TabBars.GetByKey(child->GetID("Hardware views"))) {
                    const auto desired = ImHashStr(control, 0, tabs->SelectedTabId);
                    result.focused |= context.NavId == desired;
                    result.editing |= context.ActiveId == desired;
                    for (auto& item : tabs->Tabs) {
                        const std::string_view name = ImGui::TabBarGetTabName(tabs, &item);
                        if (item.ID == tabs->SelectedTabId)
                            result.selectedTab = name;
                        if (name == tab) {
                            result.tabX = tabs->BarRect.Min.x + item.Offset + item.Width / 2;
                            result.tabY = tabs->BarRect.GetCenter().y;
                        }
                    }
                }
        if (!picture.empty()) {
            auto* surface = SDL_RenderReadPixels(SDL_GetRenderer(host), nullptr);
            Check(surface, "Inspector has a rendered picture");
            Check(SDL_SaveBMP(surface, snes::core::PathText(picture).c_str()),
                  "Save inspector picture");
            SDL_DestroySurface(surface);
        }
    });
    return result;
}

void Focus(SDL_WindowID window, const char* control) {
    for (unsigned count = 0; count < 48; ++count) {
        const auto view = Observe(window, control);
        Check(view.active, "Inspector stays open during keyboard navigation");
        if (view.focused || view.editing)
            return;
        Key(window, SDL_SCANCODE_TAB);
    }
    std::fprintf(stderr, "Cannot focus hardware control: %s\n", control);
    throw std::runtime_error("Inspector controls accept keyboard navigation");
}

void Activate(SDL_WindowID window, const char* control) {
    Focus(window, control);
    Key(window, SDL_SCANCODE_RETURN);
}

void SetText(SDL_WindowID window, const char* control, const char* text) {
    Focus(window, control);
    if (!Observe(window, control).editing)
        Key(window, SDL_SCANCODE_RETURN);
    Key(window, SDL_SCANCODE_A, SDL_KMOD_CTRL);
    Text(window, text);
    Key(window, SDL_SCANCODE_RETURN);
}

void Tab(SDL_WindowID window, const char* name) {
    const auto view = Observe(window, "", name);
    Check(view.tabX > 0, "Inspector tab is available");
    OnMain([&] { SDL_WarpMouseInWindow(SDL_GetWindowFromID(window), view.tabX, view.tabY); });
    Click(window, view.tabX, view.tabY);
    Wait([&] { return Observe(window).selectedTab == name; },
         "Inspector selects the requested tab");
}

std::vector<fs::path> Exports(const fs::path& directory, std::string_view extension) {
    std::vector<fs::path> paths;
    if (fs::is_directory(directory))
        for (const auto& entry : fs::directory_iterator(directory))
            if (entry.path().extension() == extension)
                paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
        const auto stamp = [](const auto& path) {
            const auto stem = path.stem().string();
            return std::stoull(stem.substr(stem.find_last_of('-') + 1));
        };
        return stamp(left) < stamp(right);
    });
    return paths;
}

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint64_t Field(const std::string& text, const std::string& name) {
    const auto start = text.find(name + '\t');
    Check(start != std::string::npos, "Audio export contains the requested snapshot field");
    return std::stoull(text.substr(start + name.size() + 1));
}

uint32_t Pixel(const fs::path& path, unsigned x, unsigned y) {
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> source(
        SDL_LoadBMP(snes::core::PathText(path).c_str()), SDL_DestroySurface);
    Check(bool(source), "Exported bitmap opens");
    const std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
        SDL_ConvertSurface(source.get(), SDL_PIXELFORMAT_ABGR8888), SDL_DestroySurface);
    Check(image && x < unsigned(image->w) && y < unsigned(image->h),
          "Exported bitmap contains expected pixels");
    return reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(image->pixels) +
                                             y * image->pitch)[x];
}

void Workflow(const fs::path& directory, const fs::path& pictures) {
    auto image = snes::tests::cartridge_state::Image(0x20, 2, 32768);
    std::vector<uint8_t> program;
    const auto write = [&](uint16_t address, uint8_t value) {
        for (uint8_t byte :
             {uint8_t(0xa9), value, uint8_t(0x8d), uint8_t(address), uint8_t(address >> 8)})
            program.push_back(byte);
    };
    write(0x2121, 1);
    write(0x2122, 0x1f);
    write(0x2122, 0);
    write(0x2121, 129);
    write(0x2122, 0x1f);
    write(0x2122, 0);
    write(0x2115, 0x80);
    write(0x2116, 0);
    write(0x2117, 0);
    write(0x2118, 0x80);
    write(0x2119, 0);
    program.push_back(0xdb);
    std::copy(program.begin(), program.end(), image.begin());
    const auto rom = directory / "hardware-game.sfc", profile = directory / "settings.cfg";
    {
        std::ofstream file(rom, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()), image.size());
    }
    PreferencesFile file(profile);
    auto preferences = file.Load();
    preferences.focus = FocusBehavior::AllowInput;
    file.Save(preferences);
    LaunchRequest request;
    request.paths[0] = snes::core::PathText(rom);
    StateStorage states(request);
    const auto dumps = states.Directory() / "hardware-dumps";
    if (!SDL_getenv("SNES_TEST_WINDOW"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    Check(SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    std::atomic<SDL_WindowID> window{0};
    std::atomic<bool> returned{false};
    Check(SDL_AddEventWatch(Created, &window), "Observe inspector application");
    std::exception_ptr failure;
    std::thread driver([&] {
        try {
            Wait([&] { return window.load() || returned.load(); },
                 "Application creates its window");
            Check(!returned, "Application remains available");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            Key(window, SDL_SCANCODE_P);
            Wait([&] { return Observe(window).paused; }, "Pause before inspecting hardware");
            Key(window, SDL_SCANCODE_F2);
            const auto quick0 = states.Path(StateStorage::Quick(0));
            Wait([&] { return fs::is_regular_file(quick0); }, "Capture before hardware inspection");
            const auto before = StateFile::Read(quick0);
            Key(window, SDL_SCANCODE_V, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).active && Observe(window).selectedTab == "Tiles"; },
                 "Graphics shortcut opens the tile view");
            SetText(window, "VRAM byte base (hex)", "1000000");
            Activate(window, "Apply base");
            SetText(window, "VRAM byte base (hex)", "00000");
            Activate(window, "Apply base");
            fs::create_directories(states.Directory());
            {
                std::ofstream blocker(dumps);
                Check(bool(blocker), "Create export failure fixture");
                blocker << "keep";
            }
            Activate(window, "Export view");
            Check(Read(dumps) == "keep", "Export failure retains the obstructing file");
            if (!pictures.empty())
                Observe(window, "", "", pictures / "hardware-export-error.bmp");
            fs::remove(dumps);
            Activate(window, "Export view");
            Wait([&] { return Exports(dumps, ".bmp").size() == 1; },
                 "Image export retries successfully");
            Check(Pixel(Exports(dumps, ".bmp").front(), 0, 0) == 0xff0000ff,
                  "Tile export contains the guest's actual red pixel");
            if (!pictures.empty())
                Observe(window, "", "", pictures / "hardware-tiles.bmp");
            Tab(window, "Palette");
            Activate(window, "Export view");
            Wait([&] { return Exports(dumps, ".bmp").size() == 2; }, "Palette export is available");
            Check(Pixel(Exports(dumps, ".bmp").back(), 8, 0) == 0xff0000ff,
                  "Palette export retains the guest color");
            if (!pictures.empty())
                Observe(window, "", "", pictures / "hardware-palette.bmp");
            Tab(window, "Objects");
            Activate(window, "Export view");
            Wait([&] { return Exports(dumps, ".bmp").size() == 3; }, "Object export is available");
            Check(Pixel(Exports(dumps, ".bmp").back(), 0, 0) == 0xff0000ff,
                  "Object export uses the guest's object palette");
            if (!pictures.empty())
                Observe(window, "", "", pictures / "hardware-objects.bmp");
            Activate(window, "Refresh snapshot");
            Key(window, SDL_SCANCODE_F5);
            Key(window, SDL_SCANCODE_N);
            SDL_Event drop{};
            drop.type = SDL_EVENT_DROP_FILE;
            drop.drop.windowID = window;
            drop.drop.data = request.paths[0].c_str();
            Push(drop);
            Key(window, SDL_SCANCODE_ESCAPE);
            Wait([&] { return !Observe(window).active; }, "Close graphics inspector");
            Check(Observe(window).paused, "Graphics inspector restores the pause state");
            Key(window, SDL_SCANCODE_F7);
            Key(window, SDL_SCANCODE_F2);
            const auto quick1 = states.Path(StateStorage::Quick(1));
            Wait([&] { return fs::is_regular_file(quick1); }, "Capture after hardware inspection");
            Check(StateFile::Read(quick1).machine == before.machine,
                  "Graphics views, exports, refresh and blocked commands preserve exact execution");
            Key(window, SDL_SCANCODE_A, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
            Wait([&] { return Observe(window).active && Observe(window).selectedTab == "Audio"; },
                 "Audio shortcut opens the voice view");
            Activate(window, "Export view");
            Wait([&] { return Exports(dumps, ".tsv").size() == 1; },
                 "Audio snapshot export is available");
            const auto audio = Read(Exports(dumps, ".tsv").front());
            Check(audio.find("voice\tvolume_l\tvolume_r") != std::string::npos &&
                      audio.find("register\tvalue") != std::string::npos,
                  "Audio export contains voice state and DSP registers");
            if (!pictures.empty())
                Observe(window, "", "", pictures / "hardware-audio.bmp");
            Activate(window, "Next frame");
            Activate(window, "Export view");
            Wait([&] { return Exports(dumps, ".tsv").size() == 2; },
                 "Explicit advance produces a new snapshot");
            const auto advanced = Read(Exports(dumps, ".tsv").back());
            Check(Field(advanced, "frame") == Field(audio, "frame") + 1 &&
                      Field(advanced, "master_clock") > Field(audio, "master_clock"),
                  "The inspector advances exactly one display field on request");
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
    SDL_RemoveEventWatch(Created, &window);
    SDL_Quit();
    if (failure)
        std::rethrow_exception(failure);
    Check(result == 0, "Application quits cleanly with the hardware inspector open");
}
} // namespace

int main(int argc, char** argv) {
    const auto directory =
        fs::temp_directory_path() /
        ("cupid-hardware-" +
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
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("Hardware application checks passed");
    return 0;
}
