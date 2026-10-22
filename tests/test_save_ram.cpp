// Regression coverage for battery RAM and clock persistence.

#include "ConsoleConfig.hpp"
#include "SaveRamFile.hpp"
#include "cartridge/St010CommandFixture.hpp"
#include "gameboy/BatteryProgram.hpp"
#include "snes/core/BsMemory.hpp"
#include "snes/core/Srtc.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/gameboy/Cartridge.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>

using snes::frontend::SaveRamFile;
namespace fs = std::filesystem;
namespace {
void CommandStorage(const fs::path &directory) {
  using namespace snes::core;
  const auto check = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  for (bool pal : {false, true}) {
    for (size_t savedSize : {0u, 17u, 4096u, 4103u}) {
      auto imagePath = directory / (pal ? "command-pal.sfc" : "command-ntsc.sfc");
      const auto image = st010_test::StorageGuest(pal);
      std::ofstream file(imagePath, std::ios::binary);
      file.write(reinterpret_cast<const char *>(image.data()), image.size());
      file.close();
      check(bool(file), "The command storage guest is written to disk");
      auto savePath = imagePath;
      savePath.replace_extension(".input-" + std::to_string(savedSize) + ".srm");
      if (savedSize) {
        std::vector<uint8_t> initial(savedSize);
        for (size_t i = 0; i < initial.size(); ++i)
          initial[i] = uint8_t(i * 37 + 0x53);
        SaveRamFile(savePath).Flush(initial);
      }
      SaveRamFile save(savePath);
      auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
      for (unsigned phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
          check(machine->Reset(), "The command storage guest resets");
        } else {
          check(machine->LoadCartridgeFromFile(imagePath.string()),
                "The command storage guest loads through the file API");
          check(!machine->LoadedCartridge()->HasSignalProcessor(),
                "The storage guest selects command execution without firmware");
          machine->LoadSram(save.Load(machine->LoadedCartridge()->SramLoadSize()));
        }
        for (unsigned field = 0; field < 8; ++field)
          machine->StepFrame();
        const auto *wram = machine->GetBus().WramData();
        check(wram[0x120] == 0xa5, "The command storage guest completes");
        check(std::all_of(wram + 0x10000, wram + 0x11000, [](uint8_t value) { return value == 0; }),
              "ST010 command startup ignores the battery file and clears every work RAM byte");
        std::array<uint8_t, 4096> expected{};
        expected[0] = 3;
        expected[2] = 7;
        expected[0x10] = 42;
        expected[0x20] = 6;
        expected[0x21] = 5;
        expected[0x123] = 0x69;
        check(std::equal(expected.begin(), expected.end(), wram + 0x11000),
              "The storage guest executes commands in independent work RAM");
        const auto stored = machine->LoadedCartridge()->SramData();
        check(stored.size() == 4096 && std::all_of(stored.begin(), stored.end(),
                                                   [](uint8_t value) { return value == 0; }),
              "ST010 command battery output retains the inactive processor RAM image");
        save.Flush(stored);
        check(SaveRamFile(savePath).Load(4096) == std::vector<uint8_t>(4096),
              "ST010 command saves serialize cleared processor RAM after execution and reset");
      }
    }
  }
}
void HandheldStorage(const fs::path &directory) {
  using namespace snes::core;
  const auto check = [](bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  };
  const auto write = [&](const fs::path &path, std::span<const uint8_t> data) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(data.data()), data.size());
    file.close();
    check(bool(file), "The handheld storage fixture is written to disk");
  };
  for (bool secondModel : {false, true}) {
    for (bool pal : {false, true}) {
      for (int savedSize : {-1, 0, 17, 32768}) {
        const auto casePath = directory / (std::string(secondModel ? "second" : "first") +
                                           (pal ? "-pal-" : "-ntsc-") + std::to_string(savedSize));
        fs::create_directory(casePath);
        const auto biosPath = casePath / "adapter.sfc", gamePath = casePath / "handheld.gb",
                   bootPath = casePath / "boot.rom";
        write(biosPath, snes::tests::gameboy::BatteryBios(secondModel, pal));
        write(gamePath, snes::tests::gameboy::BatteryProgram());
        write(bootPath, snes::tests::gameboy::BatteryBoot());
        auto savePath = fs::weakly_canonical(gamePath);
        savePath.replace_extension(".sav");
        std::vector<uint8_t> expected(32768, 0xff);
        if (savedSize >= 0) {
          std::vector<uint8_t> initial(savedSize);
          for (size_t index = 0; index < initial.size(); ++index)
            initial[index] = uint8_t(index * 37 + (index >> 8) * 13 + 0x53);
          write(savePath, initial);
          std::copy(initial.begin(), initial.end(), expected.begin());
        }
        std::unique_ptr<Emulator> machine;
        std::unique_ptr<SaveRamFile> save;
        for (unsigned phase = 0; phase < 3; ++phase) {
          if (phase == 1) {
            check(machine->Reset(), "The adapter storage fixture resets");
            check(machine->SaveSlotBattery(0) == expected,
                  "Warm reset retains every handheld battery byte");
          } else {
            auto config = snes::tests::DeterministicConsoleConfig();
            config.ppu.model = Ppu::RenderModel::Scanline;
            machine = std::make_unique<Emulator>(config);
            check(machine->LoadSuperGameBoyFromFiles(biosPath.string(), gamePath.string(),
                                                     bootPath.string()),
                  "The adapter, game and boot images load through the file API");
            save = std::make_unique<SaveRamFile>(savePath);
            const auto bytes = save->LoadUpTo(machine->SaveSlotBattery(0).size());
            if (!bytes.empty())
              check(machine->LoadSlotBattery(0, bytes), "The slot accepts its battery file");
            check(machine->SaveSlotBattery(0) == expected,
                  "Missing, empty, short and complete saves retain all expected RAM bytes");
          }
          for (unsigned field = 0; field < 6; ++field)
            machine->StepFrame();
          for (unsigned bank = 0; bank < 4; ++bank) {
            expected[bank * 8192 + 0x123] = uint8_t(0x53 + bank * 37);
            expected[bank * 8192 + 0x1ffe] = uint8_t(0xa0 + bank);
          }
          const auto battery = machine->SaveSlotBattery(0);
          check(battery == expected, "Handheld instructions write separate physical RAM banks");
          check(machine->SaveSlotBattery(1).empty() && !machine->LoadSlotBattery(1, battery) &&
                    machine->SaveSlotBattery(0) == expected,
                "An absent second slot cannot read or overwrite handheld battery storage");
          save->Flush(battery);
          check(SaveRamFile(savePath).Load(expected.size()) == expected,
                "The frontend save helper persists the complete slot battery image");
          const auto timestamp = fs::last_write_time(savePath) - std::chrono::hours(1);
          fs::last_write_time(savePath, timestamp);
          save->Flush(machine->SaveSlotBattery(0));
          check(fs::last_write_time(savePath) == timestamp,
                "Unchanged handheld battery data keeps the existing file");
        }
      }
    }
  }
}
} // namespace

int main() {
  const auto directory =
      fs::temp_directory_path() / ("snes-save-test-" + std::to_string(SDL_GetTicksNS()));
  if (!fs::create_directory(directory))
    return 2;
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{directory};
  try {
    auto check = [](bool condition, const char *label) {
      if (!condition)
        throw std::runtime_error(label);
    };
    const auto path = directory / "game.srm";
    SaveRamFile save(path);
    check(save.Load(8192).empty(), "A missing save starts empty");
    save.Flush({});
    check(!fs::exists(path), "A cartridge without SRAM creates no save");
    std::vector<uint8_t> data(8192, 0x57);
    save.Flush(data);
    check(SaveRamFile(path).Load(data.size()) == data, "Save round trip");
    data[4096] = 0x63;
    save.Flush(data);
    check(SaveRamFile(path).Load(data.size()) == data, "Replace an existing save");
    const auto timestamp = fs::last_write_time(path) - std::chrono::hours(1);
    fs::last_write_time(path, timestamp);
    save.Flush(data);
    check(fs::last_write_time(path) == timestamp, "Unchanged SRAM does not rewrite the save");
    bool rejected = false;
    try {
      SaveRamFile(path).Load(1024);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    check(rejected, "Reject a save with the wrong size");
    check(SaveRamFile(path).Load(data.size()) == data, "Rejecting a save preserves its contents");
    CommandStorage(directory);
    HandheldStorage(directory);
    std::vector<uint8_t> handheldImage(32768);
    handheldImage[0x147] = 0x10;
    handheldImage[0x149] = 2;
    snes::core::gameboy::Cartridge handheld;
    handheld.SetTimeSource([] { return 1700000000; });
    check(handheld.Load(handheldImage), "The handheld clock cartridge loads");
    handheld.Write(0, 0x0a);
    handheld.Write(0xa123, 0x63);
    const auto handheldData = handheld.SaveBattery();
    for (unsigned clockBytes : {0, 13, 44, 48}) {
      std::vector<uint8_t> legacy(handheldData.begin(), handheldData.begin() + 8192);
      legacy.resize(8192 + clockBytes);
      if (clockBytes == 13) {
        for (unsigned digit = 0; digit < 5; ++digit)
          legacy[8192 + digit] = handheldData[8192 + digit * 4];
        std::copy_n(handheldData.begin() + 8192 + 40, 8, legacy.begin() + 8192 + 5);
      } else if (clockBytes)
        std::copy_n(handheldData.begin() + 8192, clockBytes, legacy.begin() + 8192);
      const auto handheldPath = directory / ("handheld-" + std::to_string(clockBytes) + ".sav");
      SaveRamFile handheldSave(handheldPath);
      handheldSave.Flush(legacy);
      const auto loaded = handheldSave.LoadUpTo(handheldData.size());
      check(loaded == legacy, "Supported handheld battery layouts retain their original bytes");
      snes::core::gameboy::Cartridge restored;
      restored.SetTimeSource([] { return 1700000000; });
      check(restored.Load(handheldImage), "The handheld restore cartridge loads");
      restored.LoadBattery(loaded);
      const auto updated = restored.SaveBattery();
      check(updated.size() == handheldData.size() && updated[0x123] == 0x63,
            "Legacy handheld saves retain RAM when their clock trailer is upgraded");
      if (clockBytes)
        check(updated == handheldData, "Legacy clock layouts retain the full calendar state");
      handheldSave.Flush(updated);
      check(SaveRamFile(handheldPath).Load(updated.size()) == updated,
            "An upgraded handheld battery replaces its earlier file atomically");
    }
    int64_t now = 1700000000;
    snes::core::Srtc clock([&] { return now; });
    clock.Write(14);
    clock.Write(0);
    for (uint8_t digit : {9, 5, 9, 5, 3, 2, 1, 3, 12, 3, 2, 10})
      clock.Write(digit);
    const auto rtcPath = directory / "game.rtc";
    const auto rtcData = clock.Save();
    SaveRamFile(rtcPath).Flush(rtcData);
    ++now;
    snes::core::Srtc resumed([&] { return now; });
    check(resumed.Load(SaveRamFile(rtcPath).Load(snes::core::Srtc::SaveSize)),
          "Clock sidecar reload");
    resumed.Write(13);
    check(resumed.Read() == 15 && resumed.Read() == 0, "Clock advances across file reload");
    const std::vector<uint8_t> slotA(0x4000, 0x57), slotB(0x4000, 0x63);
    SaveRamFile(directory / "slot-a.srm").Flush(slotA);
    SaveRamFile(directory / "slot-b.srm").Flush(slotB);
    check(SaveRamFile(directory / "slot-a.srm").Load(0x4000) == slotA &&
              SaveRamFile(directory / "slot-b.srm").Load(0x4000) == slotB,
          "Slot save files remain independent");
    const auto flashPath = directory / "game.flash";
    const auto metadataPath = directory / "game.flash.meta";
    std::vector<uint8_t> flashImage(0x100000, 0x00);
    snes::core::BsMemory flash(flashImage, false);
    flash.SetWritable(true);
    flash.Write(0, 0x20);
    flash.Write(0x1234, 0xd0);
    flash.Advance(snes::core::Timing::kMasterClockHz * 3 / 10 + 1);
    flash.Write(0, 0x40);
    flash.Write(0x1234, 0x5a);
    const auto metadata = flash.Metadata();
    check(metadata[16] == 2 && metadata[20] == 0 && flashImage[0x1234] == 0x5a,
          "Erase and program change flash data and physical metadata");
    SaveRamFile(flashPath).Flush(flashImage);
    SaveRamFile(metadataPath).Flush(metadata);
    auto restoredImage = SaveRamFile(flashPath).Load(flashImage.size());
    snes::core::BsMemory restoredFlash(restoredImage, false);
    check(restoredImage == flashImage &&
              restoredFlash.LoadMetadata(SaveRamFile(metadataPath).Load(metadata.size())) &&
              restoredFlash.Metadata() == metadata,
          "Flash array and metadata survive independent sidecar reloads");
    restoredFlash.Write(0, 0x97);
    restoredFlash.Write(0, 0xd0);
    restoredFlash.Write(0, 0x40);
    restoredFlash.Write(0x1234, 0x0f);
    check(restoredImage[0x1234] == 0x0a && restoredFlash.Metadata() == metadata,
          "Restored physical locks control later writes without the external write pin");
    const auto blocked = directory / "directory.srm";
    fs::create_directory(blocked);
    std::ofstream(blocked / "keep.txt") << "preserve";
    rejected = false;
    try {
      SaveRamFile(blocked).Flush(data);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    check(rejected && fs::exists(blocked / "keep.txt"),
          "A failed replacement preserves the destination");
    for (const auto &entry : fs::directory_iterator(directory))
      check(entry.path().filename().string().find(".tmp-") == std::string::npos,
            "Save operations clean up temporary files");
    std::printf("Save RAM persistence checks passed\n");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
