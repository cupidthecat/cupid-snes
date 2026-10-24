#include "HardwareDialog.hpp"
#include "imgui.h"
#include "imgui_stdlib.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <utility>

namespace snes::frontend {
namespace {
uint32_t Address(std::string_view text) {
    if (text.starts_with('$'))
        text.remove_prefix(1);
    else if (text.starts_with("0x") || text.starts_with("0X"))
        text.remove_prefix(2);
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Enter a hexadecimal video RAM byte address");
    return value;
}

int Signed(uint8_t value) {
    return value & 128 ? int(value) - 256 : int(value);
}
} // namespace

struct HardwareDialog::Impl {
    SDL_Renderer* renderer;
    SDL_Texture* preview = nullptr;
    HardwareSnapshot snapshot;
    core::VideoFrame picture;
    std::optional<core::VideoFrame> pendingPicture;
    std::optional<Export> exportRequest;
    std::string message, base = "00000", pictureLabel;
    int format = 1, page = 0, palette = 0, selectedColor = 0, selectedObject = 0, selectedVoice = 0;
    int zoom = 2, objectZoom = 16, currentTab = -1;
    bool active = false, opening = false, audioTab = false, selectTab = false;
    bool refresh = false, step = false, dirty = true, currentPreview = false;
    bool transparentZero = true, applyFlips = true;
    bool fitObject = true;
    uint32_t baseByte = 0;

    ~Impl() {
        SDL_DestroyTexture(preview);
    }

    void Attempt(const std::function<void()>& action) {
        try {
            action();
            message.clear();
        } catch (const std::exception& error) {
            message = error.what();
        }
    }

    void SetPicture(core::VideoFrame next, std::string label) {
        picture = std::move(next);
        pictureLabel = std::move(label);
        pendingPicture = picture;
        currentPreview = false;
        dirty = false;
    }

    void ApplyPicture() {
        auto next = std::exchange(pendingPicture, {});
        if (!next)
            return;
        SDL_DestroyTexture(std::exchange(preview, nullptr));
        Attempt([&] {
            preview =
                SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                  int(next->width), int(next->height));
            if (!preview ||
                !SDL_UpdateTexture(preview, nullptr, next->pixels.data(),
                                   int(next->width * sizeof(uint32_t))) ||
                !SDL_SetTextureScaleMode(preview, SDL_SCALEMODE_NEAREST)) {
                const std::string error = SDL_GetError();
                SDL_DestroyTexture(std::exchange(preview, nullptr));
                throw std::runtime_error(error);
            }
            currentPreview = true;
        });
    }

    void Select(int tab) {
        if (currentTab != tab) {
            currentTab = tab;
            dirty = true;
            currentPreview = false;
        }
    }

    void Image() {
        if (currentPreview && preview) {
            const auto scale =
                currentTab == 2
                    ? (fitObject ? std::max(1u, 256u / std::max(picture.width, picture.height))
                                 : unsigned(objectZoom))
                    : unsigned(zoom);
            ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(preview)),
                         {float(picture.width * scale), float(picture.height * scale)});
        } else
            ImGui::TextDisabled("Updating preview");
    }

    void Palette() {
        ImGui::SetNextItemWidth(220);
        ImGui::SliderInt("Color index", &selectedColor, 0, 255);
        const auto raw = snapshot.cgram[size_t(selectedColor)];
        const auto color = HardwareSnapshot::Color(raw);
        ImGui::Text("CGRAM %02X: $%04X   R %u  G %u  B %u", selectedColor, unsigned(raw),
                    color & 255, (color >> 8) & 255, (color >> 16) & 255);
        if (ImGui::Button("Copy color")) {
            char text[32];
            std::snprintf(text, sizeof(text), "%02X: %04X", selectedColor, unsigned(raw));
            ImGui::SetClipboardText(text);
        }
        if (dirty)
            Attempt([&] {
                core::VideoFrame image{128, 128, {}};
                image.pixels.resize(128 * 128);
                for (unsigned y = 0; y < 128; ++y)
                    for (unsigned x = 0; x < 128; ++x)
                        image.pixels[y * 128 + x] =
                            HardwareSnapshot::Color(snapshot.cgram[(y / 8) * 16 + x / 8]);
                SetPicture(std::move(image), "palette");
            });
        Image();
        if (currentPreview) {
            const auto start = ImGui::GetItemRectMin();
            if (ImGui::IsItemHovered()) {
                const auto mouse = ImGui::GetMousePos();
                const auto x = std::clamp(int((mouse.x - start.x) / (8 * zoom)), 0, 15);
                const auto y = std::clamp(int((mouse.y - start.y) / (8 * zoom)), 0, 15);
                const auto index = y * 16 + x;
                const bool clicked = ImGui::IsItemClicked();
                ImGui::SetTooltip("CGRAM %02X: $%04X", index,
                                  unsigned(snapshot.cgram[size_t(index)]));
                if (clicked)
                    selectedColor = index;
            }
            const ImVec2 minimum{start.x + (selectedColor % 16) * 8 * zoom,
                                 start.y + (selectedColor / 16) * 8 * zoom};
            ImGui::GetWindowDrawList()->AddRect(minimum,
                                                {minimum.x + 8 * zoom, minimum.y + 8 * zoom},
                                                IM_COL32(255, 255, 255, 255), 0, 0, 2);
        }
        ImGui::TextWrapped("Raw CGRAM colors. Display brightness and color math are not applied.");
    }

    void Tiles() {
        ImGui::SetNextItemWidth(130);
        if (ImGui::Combo("Format", &format,
                         "2 bpp\0"
                         "4 bpp\0"
                         "8 bpp\0"
                         "Mode 7 packed\0")) {
            page = palette = 0;
            dirty = true;
        }
        const auto tileFormat = HardwareSnapshot::TileFormat(format);
        const auto pages = int(snapshot.TileCount(tileFormat) / 256);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        dirty |= ImGui::SliderInt("Tile page", &page, 0, pages - 1);
        ImGui::SetNextItemWidth(100);
        ImGui::InputText("VRAM byte base (hex)", &base);
        ImGui::SameLine();
        if (ImGui::Button("Apply base"))
            Attempt([&] {
                const auto address = Address(base);
                if (address >= snapshot.vram.size() * 2 || (address & 1))
                    throw std::out_of_range("Choose an even byte address inside video RAM");
                baseByte = address;
                dirty = true;
            });
        const auto banks = int(256u >> HardwareSnapshot::Depth(tileFormat));
        ImGui::BeginDisabled(banks == 1);
        ImGui::SetNextItemWidth(180);
        dirty |= ImGui::SliderInt("Palette bank", &palette, 0, banks - 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        dirty |= ImGui::Checkbox("Transparent color zero", &transparentZero);
        if (dirty)
            Attempt([&] {
                SetPicture(snapshot.Tiles({tileFormat, baseByte, unsigned(page) * 256,
                                           unsigned(palette), transparentZero}),
                           "tiles");
            });
        Image();
        ImGui::TextWrapped("Raw tile storage. Checkerboard pixels represent color zero when "
                           "transparency is enabled.");
    }

    void Objects() {
        ImGui::SetNextItemWidth(180);
        dirty |= ImGui::SliderInt("Object", &selectedObject, 0, 127);
        ImGui::SameLine();
        dirty |= ImGui::Checkbox("Apply flips", &applyFlips);
        ImGui::SameLine();
        dirty |= ImGui::Checkbox("Transparent zero", &transparentZero);
        const auto objects = snapshot.Objects();
        const auto& object = objects[size_t(selectedObject)];
        ImGui::Text("X %d (raw %u)  Y %u (raw %u)  Size %ux%u  Tile %03X", object.x, object.rawX,
                    object.y, object.rawY, object.width, object.height, object.tile);
        ImGui::Text("Palette %u  Priority %u  Large %u  Flip X %u / Y %u", object.palette,
                    object.priority, unsigned(object.large), unsigned(object.flipX),
                    unsigned(object.flipY));
        if (dirty)
            Attempt([&] {
                SetPicture(
                    snapshot.ObjectImage(unsigned(selectedObject), applyFlips, transparentZero),
                    "object-" + std::to_string(selectedObject));
            });
        const auto width = std::min(300.0f, ImGui::GetContentRegionAvail().x * 0.48f);
        if (ImGui::BeginTable("Object attributes", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY,
                              {width, 220})) {
            for (const auto* label : {"Index", "X", "Y", "Tile"})
                ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
            ImGuiListClipper clipper;
            clipper.Begin(128);
            while (clipper.Step())
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                    const auto& row = objects[size_t(index)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID(index);
                    char label[8];
                    std::snprintf(label, sizeof(label), "%d", index);
                    if (ImGui::Selectable(label, index == selectedObject,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        selectedObject = index;
                        dirty = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", row.x);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", row.y);
                    ImGui::TableNextColumn();
                    ImGui::Text("%03X", row.tile);
                    ImGui::PopID();
                }
            ImGui::PopItemFlag();
            ImGui::EndTable();
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Checkbox("Fit object preview", &fitObject);
        Image();
        ImGui::EndGroup();
        ImGui::TextWrapped("Raw object tiles at full height. Scanline limits, display interlace, "
                           "priority and color math are not applied.");
    }

    void Audio() {
        ImGui::Text("Console DSP phase %u  Counter %d  Noise %04X", snapshot.dspPhase,
                    snapshot.counter, unsigned(snapshot.noise));
        ImGui::Text("Main L/R %d/%d  Echo L/R %d/%d  KON %02X  KOFF %02X  Flags %02X",
                    Signed(snapshot.dsp[0x0c]), Signed(snapshot.dsp[0x1c]),
                    Signed(snapshot.dsp[0x2c]), Signed(snapshot.dsp[0x3c]),
                    unsigned(snapshot.dsp[0x4c]), unsigned(snapshot.dsp[0x5c]),
                    unsigned(snapshot.dsp[0x6c]));
        ImGui::SetNextItemWidth(200);
        ImGui::SliderInt("Voice", &selectedVoice, 0, 7);
        if (ImGui::BeginTable("Audio voices", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg,
                              {0, 0})) {
            for (const auto* label :
                 {"Voice", "Vol L", "Vol R", "Pitch", "Source", "Envelope", "BRR"})
                ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            for (unsigned index = 0; index < 8; ++index) {
                const auto offset = index * 16;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", index);
                ImGui::TableNextColumn();
                ImGui::Text("%d", Signed(snapshot.dsp[offset]));
                ImGui::TableNextColumn();
                ImGui::Text("%d", Signed(snapshot.dsp[offset + 1]));
                ImGui::TableNextColumn();
                ImGui::Text("%04X", (unsigned(snapshot.dsp[offset + 3] & 63) << 8) |
                                        snapshot.dsp[offset + 2]);
                ImGui::TableNextColumn();
                ImGui::Text("%02X", unsigned(snapshot.dsp[offset + 4]));
                ImGui::TableNextColumn();
                ImGui::Text("%03X", unsigned(snapshot.voices[index].env));
                ImGui::TableNextColumn();
                ImGui::Text("%04X", unsigned(snapshot.voices[index].brrAddr));
            }
            ImGui::EndTable();
        }
        const auto samples = snapshot.DecodedSamples(unsigned(selectedVoice));
        ImGui::PlotLines("Decoded ring (12 samples)", samples.data(), int(samples.size()), 0,
                         nullptr, -1, 1, {0, 70});
        const auto& voice = snapshot.voices[size_t(selectedVoice)];
        constexpr const char* modes[]{"Release", "Attack", "Decay", "Sustain"};
        ImGui::Text("Voice %d: %s  Output %d  Start %04X  Loop %04X", selectedVoice,
                    modes[std::clamp(int(voice.envMode), 0, 3)], voice.output,
                    unsigned(snapshot.DirectoryAddress(unsigned(selectedVoice), false)),
                    unsigned(snapshot.DirectoryAddress(unsigned(selectedVoice), true)));
        ImGui::TextWrapped(
            "The ring shows recent decoded samples, before interpolation and mixing.");
        if (ImGui::CollapsingHeader("DSP registers"))
            for (unsigned row = 0; row < 8; ++row) {
                std::string text;
                char byte[16];
                std::snprintf(byte, sizeof(byte), "%02X: ", row * 16);
                text += byte;
                for (unsigned column = 0; column < 16; ++column) {
                    std::snprintf(byte, sizeof(byte), "%02X ",
                                  unsigned(snapshot.dsp[row * 16 + column]));
                    text += byte;
                }
                ImGui::TextUnformatted(text.c_str());
            }
    }
};

HardwareDialog::HardwareDialog(SDL_Renderer* renderer) : impl_(std::make_unique<Impl>()) {
    impl_->renderer = renderer;
}

HardwareDialog::~HardwareDialog() = default;

void HardwareDialog::Open(HardwareSnapshot snapshot, bool audio) {
    auto& self = *impl_;
    self.snapshot = std::move(snapshot);
    self.active = self.opening = self.selectTab = self.dirty = true;
    self.currentPreview = false;
    self.audioTab = audio;
    self.currentTab = -1;
    self.message.clear();
    self.refresh = self.step = false;
    self.exportRequest.reset();
    self.pendingPicture.reset();
    self.picture = {};
    self.pictureLabel.clear();
    self.baseByte = 0;
    self.base = "00000";
    self.page = self.palette = 0;
}

void HardwareDialog::Updated(HardwareSnapshot snapshot) {
    impl_->snapshot = std::move(snapshot);
    impl_->dirty = true;
    impl_->currentPreview = false;
    impl_->pendingPicture.reset();
    impl_->message.clear();
}

bool HardwareDialog::Active() const {
    return impl_->active;
}

bool HardwareDialog::TakeRefresh() {
    return std::exchange(impl_->refresh, false);
}

bool HardwareDialog::TakeStep() {
    return std::exchange(impl_->step, false);
}

std::optional<HardwareDialog::Export> HardwareDialog::TakeExport() {
    return std::exchange(impl_->exportRequest, {});
}

void HardwareDialog::Message(std::string message) {
    impl_->message = std::move(message);
}

void HardwareDialog::Draw() {
    auto& self = *impl_;
    if (!self.active)
        return;
    self.ApplyPicture();
    if (self.opening) {
        ImGui::OpenPopup("Graphics and audio");
        self.opening = false;
    }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(
        {std::min(900.0f, viewport->Size.x - 24), std::min(680.0f, viewport->Size.y - 48)},
        ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Graphics and audio", &self.active)) {
        ImGui::Text("Snapshot: frame %llu, master clock %llu, VRAM %zu KiB",
                    static_cast<unsigned long long>(self.snapshot.frame),
                    static_cast<unsigned long long>(self.snapshot.clock),
                    self.snapshot.vram.size() / 512);
        if (ImGui::Button("Refresh snapshot"))
            self.refresh = true;
        ImGui::SameLine();
        if (ImGui::Button("Next frame"))
            self.step = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(self.currentTab != 3 && !self.currentPreview);
        if (ImGui::Button("Export view"))
            self.Attempt([&] {
                self.exportRequest = self.currentTab == 3
                                         ? Export{"audio", {}, self.snapshot.AudioText()}
                                         : Export{self.pictureLabel, self.picture, {}};
            });
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        const bool fitted = self.currentTab == 2 && self.fitObject;
        int displayZoom =
            fitted && self.picture.width && self.picture.height
                ? int(std::max(1u, 256u / std::max(self.picture.width, self.picture.height)))
                : (self.currentTab == 2 ? self.objectZoom : self.zoom);
        ImGui::BeginDisabled(fitted || self.currentTab == 3);
        if (ImGui::SliderInt("Zoom", &displayZoom, 1, self.currentTab == 2 ? 32 : 4, "%dx"))
            (self.currentTab == 2 ? self.objectZoom : self.zoom) = displayZoom;
        ImGui::EndDisabled();
        ImGui::TextWrapped("Playback is paused. Next frame advances one display field. Keyboard "
                           "and pointer controls operate this window.");
        const auto footer = ImGui::GetFrameHeightWithSpacing() + (self.message.empty() ? 0 : 56);
        if (ImGui::BeginChild("Hardware content", {0, -footer}, ImGuiChildFlags_NavFlattened)) {
            if (ImGui::BeginTabBar("Hardware views")) {
                const auto tab = [&](const char* label, int id, bool select, auto draw) {
                    if (ImGui::BeginTabItem(label, nullptr,
                                            select ? ImGuiTabItemFlags_SetSelected : 0)) {
                        self.Select(id);
                        draw();
                        ImGui::EndTabItem();
                    }
                };
                tab("Palette", 0, false, [&] { self.Palette(); });
                tab("Tiles", 1, self.selectTab && !self.audioTab, [&] { self.Tiles(); });
                tab("Objects", 2, false, [&] { self.Objects(); });
                tab("Audio", 3, self.selectTab && self.audioTab, [&] { self.Audio(); });
                ImGui::EndTabBar();
                self.selectTab = false;
            }
        }
        ImGui::EndChild();
        if (!self.message.empty()) {
            if (ImGui::BeginChild("Hardware message", {0, 48}, ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoNav))
                ImGui::TextWrapped("%s", self.message.c_str());
            ImGui::EndChild();
        }
        if (ImGui::Button("Close", {100, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            self.active = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
} // namespace snes::frontend
