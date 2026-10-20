#include "snes/core/detail/Compatibility.hpp"

#include "snes/core/Cartridge.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace snes::core::detail {

bool PalCountry(uint8_t country) noexcept {
  return country != 0 && country != 1 && country != 0x0b && country != 0x0d && country != 0x0f &&
         country != 0x10;
}

CompatibilitySettings SelectCompatibility(const RomHeader &header, Ppu::Config video,
                                          EntropyMode entropy, bool hotfixes) {
  CompatibilitySettings result;
  result.video = video;
  result.entropy = entropy;
  const std::string_view title = header.title;
  constexpr std::array<std::string_view, 6> progressiveTitles{
      "AIR STRIKE PATROL",
      "DESERT FIGHTER",
      "Winter olympics",
      "WORLD CUP STRIKER",
      "\xe3\x83\x9e\xe3\x83\xbc\xe3\x83\xb4\xe3\x82\xa7\xe3\x83\xa9\xe3\x82\xb9",
      "SFC \xe3\x82\xaf\xe3\x83\xac\xe3\x83\xa8\xe3\x83\xb3\xe3\x82\xb7\xe3\x83\xb3"
      "\xe3\x83\x81\xe3\x83\xa3\xe3\x83\xb3"};
  if (std::find(progressiveTitles.begin(), progressiveTitles.end(), title) !=
      progressiveTitles.end())
    result.video.model = Ppu::RenderModel::Progressive;
  constexpr std::array<std::string_view, 4> earlyTitles{"FIREPOWER 2000", "SUPER SWIV", "NHL '94",
                                                        "NHL PROHOCKEY'94"};
  if (std::find(earlyTitles.begin(), earlyTitles.end(), title) != earlyTitles.end() ||
      (title == "ADVENTURES OF FRANKEN" && PalCountry(header.country)))
    result.video.renderCycle = 32;
  if (title == "Sugoro Quest++")
    result.video.renderCycle = 128;
  if (hotfixes) {
    result.initializeWram = title == "DIRT RACER";
    result.initializeDsp = title == "MAGICAL DROP";
    if (title == "RENDERING RANGER R2")
      result.sampleRate = 32000;
    constexpr std::string_view arcade =
        "\xe3\x83\x8b\xe3\x83\x81\xe3\x83\x96\xe3\x83\x84\xe3\x83\xbb\xe3\x82\xa2"
        "\xe3\x83\xbc\xe3\x82\xb1\xe3\x83\xbc\xe3\x83\x89\xe3\x83\xbb\xe3\x82\xaf"
        "\xe3\x83\xa9\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf\xe3\x82\xb9";
    if (title == "The Hurricanes" || title == arcade)
      result.entropy = EntropyMode::None;
  }
  return result;
}

} // namespace snes::core::detail
