// Regression coverage for cartridge mappers and enhancement devices.

#include "ConsoleConfig.hpp"
#include "snes/core/SnesCpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <vector>

using namespace snes::core;
namespace {
unsigned checks = 0;
void Check(bool value, const char *label) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    throw std::runtime_error(label);
  }
}

std::shared_ptr<CartridgeFirmware> DetectionFirmware() {
  auto firmware = std::make_shared<CartridgeFirmware>();
  for (auto kind : {FirmwareKind::Dsp3, FirmwareKind::St011, FirmwareKind::St018})
    Check(firmware->Set(kind, std::vector<uint8_t>(CartridgeFirmware::Size(kind))),
          "Header fixtures provide complete zero-operation processor images");
  return firmware;
}

std::vector<uint8_t> Rom(size_t size = 0x100000, uint8_t mode = 0x20, uint8_t type = 0) {
  std::vector<uint8_t> bytes(size, 0xea);
  const size_t header = mode == 0x35 || mode == 0x25     ? 0x40ffc0
                        : (mode & 1) && (mode & 15) == 1 ? 0xffc0
                                                         : 0x7fc0;
  std::fill_n(bytes.begin() + header - 16, 80, 0);
  std::fill_n(bytes.begin() + header, 21, ' ');
  bytes[header] = 'T';
  bytes[header + 0x15] = mode;
  bytes[header + 0x16] = type;
  bytes[header + 0x17] = 10;
  bytes[header + 0x19] = 1;
  bytes[header + 0x1c] = 0x34;
  bytes[header + 0x1d] = 0x12;
  bytes[header + 0x1e] = 0xcb;
  bytes[header + 0x1f] = 0xed;
  bytes[header + 0x3d] = 0x80;
  return bytes;
}

std::unique_ptr<Emulator> Load(const std::vector<uint8_t> &bytes) {
  auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  std::string error;
  if (!emu->LoadCartridge(bytes, &error))
    throw std::runtime_error(error);
  return emu;
}

void IdentifiedSlotCartridges() {
  auto ordinary = Rom();
  const std::string_view biosTitle = "Satellaview BS-X     ";
  std::copy(biosTitle.begin(), biosTitle.end(), ordinary.begin() + 0x7fc0);
  auto cart = Cartridge::FromRomImage(ordinary, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().mapping == MappingType::LoRom && cart->SramData().empty() &&
            cart->Read(0x025000, 0xa5) == 0xa5,
        "A BIOS title alone does not connect a broadcast controller");

  for (const auto [mode, expected] : {std::tuple{uint8_t(0x20), MappingType::BroadcastLoRom},
                                      std::tuple{uint8_t(0x21), MappingType::BroadcastHiRom},
                                      std::tuple{uint8_t(0x23), MappingType::BroadcastSa1}}) {
    const size_t header = mode == 0x21 ? 0xffc0 : 0x7fc0;
    auto bytes = Rom(0x100000, mode);
    std::copy_n("ZGNJ", 4, bytes.begin() + header - 14);
    bytes[header + 0x1a] = 0x33;
    bytes[header + 0x18] = 3;
    cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && cart->BoardConnected() && cart->Header().mapping == expected &&
              cart->SramData().size() == 8192 && cart->MemoryPackData().empty(),
          "A valid identifier selects its slot board without inserting a memory pack");
    if (mode != 0x23) {
      Check(cart->Read(mode == 0x21 ? 0x208123 : 0xc08123, 0x49) == 0x49,
            "An absent ordinary memory slot preserves open bus");
      auto machine = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
      Check(machine->LoadBroadcastCartridge(bytes, {}), "An explicit empty slot loads");
      machine->GetBus().Write(0x7e0000, 0x5b);
      Check(machine->LoadedCartridge()->MemoryPackData().empty() &&
                machine->GetBus().Read(mode == 0x21 ? 0x208123 : 0xc08123) == 0x5b,
            "Explicit slot loading retains physical absence");
    }
    bytes[header + 0x18] = 0;
    cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && !cart->BoardConnected() && cart->SramData().empty() &&
              cart->Read(0x008000, 0x79) == 0x79,
          "Slot profiles without their declared RAM remain unconnected");
  }

  for (const uint8_t declaration : {3, 5, 7, 8}) {
    auto bytes = Rom();
    std::copy_n("ZBSJ", 4, bytes.begin() + 0x7fb2);
    bytes[0x7fda] = 0x33;
    bytes[0x7fd8] = declaration;
    cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && cart->Header().mapping == MappingType::Bsx &&
              cart->SramData().size() == (size_t(1024) << declaration) &&
              cart->Read(0x025000, 0) == 0x80 && cart->MemoryPackData().empty(),
          "The mapper identifier selects hardware and retains its declared save capacity");
    Check(cart->Read(0xc08123, 0x96) == 0x96,
          "The mapper leaves an absent memory pack electrically unconnected");
    if (declaration >= 7) {
      cart->Write(0x105123, 0x47);
      cart->Write(0x175abc, 0x63);
      Check(cart->SramData()[0x10123] == 0x47 && cart->SramData()[0x17abc] == 0x63 &&
                cart->SramData()[0x123] == 0xff,
            "The mapper save window retains its connected bank address lines");
    }
  }

  auto bytes = Rom();
  std::copy_n("ZBSJ", 4, bytes.begin() + 0x7fb2);
  bytes[0x7fda] = 0x33;
  cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(cart && !cart->BoardConnected() && cart->Read(0x025000, 0x47) == 0x47,
        "The mapper profile requires save RAM before connecting its registers");
  bytes[0x7fd8] = 3;
  bytes[0x7fda] = 1;
  cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().mapping == MappingType::LoRom && cart->Read(0x025000, 0x47) == 0x47,
        "Legacy maker metadata does not enable an extended identifier");

  for (const std::string_view identifier : {"ZgNJ", "ZGNE", "Z NJ", "ZG?J", "AGNJ"}) {
    bytes = Rom();
    std::copy(identifier.begin(), identifier.end(), bytes.begin() + 0x7fb2);
    bytes[0x7fda] = 0x33;
    bytes[0x7fd8] = 3;
    cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && cart->Header().mapping == MappingType::LoRom,
          "Invalid or unrelated identifiers retain the ordinary board");
  }

  for (const uint8_t mode : {0x20, 0x21, 0x23}) {
    const size_t header = mode == 0x21 ? 0xffc0 : 0x7fc0;
    bytes = Rom(0x100000, mode, 0xf6);
    std::copy_n("ZBSJ", 4, bytes.begin() + header - 14);
    bytes[header + 0x1a] = 0x33;
    bytes[header + 0x18] = 0;
    bytes[header - 3] = 7;
    bytes[header - 1] = 2;
    cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && cart->Header().mapping == MappingType::Bsx &&
              cart->Header().chip == EnhancementChip::None &&
              cart->RomData().size() == bytes.size() && cart->SramData().size() == 0x20000,
          "A valid mapper ID precedes processor and firmware selection and accepts expansion RAM");
  }

  bytes = Rom(0x100000, 0x22, 0x45);
  std::copy_n("ZGNJ", 4, bytes.begin() + 0x7fb2);
  bytes[0x7fda] = 0x33;
  bytes[0x7fd8] = 3;
  cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(cart && !cart->BoardConnected() && cart->Header().chip == EnhancementChip::None,
        "A slot identifier with unsupported wiring does not connect the raw device family");

  bytes = Rom();
  std::copy_n("ZBSJ", 4, bytes.begin() + 0x7fb2);
  bytes[0x7fda] = 0x33;
  CartridgeDatabase database;
  DatabaseOverride override;
  override.forceMapping = MappingType::HiRom;
  override.forceSramSize = 0x2000;
  cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  database.AddOverride(cart->RomCrc32(), override);
  cart = Cartridge::FromRomImage(bytes, nullptr, &database, nullptr);
  Check(cart && cart->BoardConnected() && cart->Header().mapping == MappingType::HiRom &&
            cart->SramData().size() == 0x2000 && cart->Read(0x025000, 0x47) == 0x47,
        "An explicit database mapping takes precedence over automatic board selection");

  bytes = Rom(0x200000);
  std::copy_n("ZBSJ", 4, bytes.begin() + 0x7fb2);
  bytes[0x7fda] = 0x33;
  bytes[0x7fd8] = 3;
  bytes[0x123] = 0x25;
  bytes[0x100123] = 0xb7;
  cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(cart && cart->Read(0x008123) == 0x25 && cart->Read(0x208123) == 0xb7 &&
            cart->Read(0x808123) == 0x25 && cart->Read(0xa08123) == 0xb7,
        "Mapper BIOS windows retain the bank address line above one MiB");

  bytes = Rom(0x128000, 0x20, 0xf6);
  std::copy_n("ZGNJ", 4, bytes.begin() + 0x7fb2);
  bytes[0x7fda] = 0x33;
  bytes[0x7fd8] = 3;
  bytes[0x7fbf] = 2;
  cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().chip == EnhancementChip::None &&
            cart->RomData().size() == 0x100000 && !cart->HasArmProcessor(),
        "A slot ID removes declared firmware trailers without connecting their processor");

  for (const char *identifier : {"ZGNJ", "ZX3J", "ZBPJ"}) {
    bytes = Rom(0x100000, 0x23);
    std::copy_n(identifier, 4, bytes.begin() + 0x7fb2);
    bytes[0x7fda] = 0x33;
    bytes[0x7fd8] = 3;
    cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && cart->Header().mapping == MappingType::BroadcastSa1,
          "SA-1 slot selection accepts every valid broadcast identifier");
    Check(cart->Read(0x500123, 0xa5) == 0xa5 && cart->Read(0x600123, 0xa5) == 0xa5,
          "A memory slot does not enlarge the CPU's SA-1 RAM window");
    for (unsigned page = 4; page < 8; ++page) {
      cart->Write(0x002220, uint8_t(0x80 | page));
      Check(cart->Read(0xc08123, 0xa5) == bytes[0x8123],
            "An absent SA-1 expansion retains physical program-ROM mirroring");
    }
  }
}

void CompleteBoardProfiles() {
  const std::array<uint8_t, 8> program{0x78, 0xa9, 0x5a, 0x8f, 0, 0x10, 0x7e, 0xdb};
  for (const auto [size, mode, type, ram] :
       {std::tuple{size_t(0x8000), uint8_t(0x30), uint8_t(0x55), uint8_t(3)},
        std::tuple{size_t(0x100000), uint8_t(0x30), uint8_t(0x25), uint8_t(0)},
        std::tuple{size_t(0x700000), uint8_t(0x3a), uint8_t(0xf5), uint8_t(3)}}) {
    const size_t base = mode == 0x3a ? 0xffc0 : 0x7fc0;
    auto bytes = Rom(size, mode == 0x3a ? 0x21 : mode, type);
    bytes[base + 0x15] = mode;
    bytes[base + 0x18] = ram;
    std::copy(program.begin(), program.end(), bytes.begin() + (base & ~size_t(0x7fff)));
    auto cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && cart->Read(0x008000, 0xa5) == 0xa5 && cart->SramData().empty(),
          "An unconnected board profile retains open bus and has no save storage");
    auto machine = Load(bytes);
    std::fill_n(machine->GetBus().WramData(), MemoryBus::WramSize, 0);
    machine->StepFrame();
    Check(machine->GetBus().Read(0x7e1000) == 0,
          "An unsupported profile cannot execute a cartridge reset program");
  }

  auto bytes = Rom(0x100000, 0x21, 0xf5);
  bytes[0xffd5] = 0x3a;
  bytes[0xffd8] = 3;
  std::copy(program.begin(), program.end(), bytes.begin() + 0x8000);
  auto machine = Load(bytes);
  std::fill_n(machine->GetBus().WramData(), MemoryBus::WramSize, 0);
  machine->StepFrame();
  Check(machine->GetBus().Read(0x7e1000) == 0x5a,
        "A program-only SPC7110 board boots without a data image");
  Check(machine->GetBus().Read(0xd00000) == 0, "An absent SPC7110 data image supplies zero data");

  bytes = Rom(0x100000, 0x30, 0x25);
  bytes[0x7fd8] = 7;
  auto cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  Check(cart && cart->SramData().size() == 0x20000,
        "OBC1 retains the declared save capacity beyond its decoded port window");
  cart->Write(0x006123, 0x47);
  Check(cart->SramData()[0x123] == 0x47 && cart->SramData().back() == 0xff,
        "OBC1 writes preserve the unused declared save storage");
}

void HeaderRamDeclarations() {
  for (const auto [type, ordinary, expansion, bytes] :
       {std::tuple{0x02, 0, 5, size_t(0x8000)}, std::tuple{0x02, 3, 7, size_t(0x2000)},
        std::tuple{0x15, 0, 0, size_t(0x8000)}, std::tuple{0x15, 7, 8, size_t(0x20000)},
        std::tuple{0x10, 0, 0, size_t(0x8000)}, std::tuple{0x02, 0, 0xf3, size_t(0x2000)},
        std::tuple{0x02, 0, 0x0f, size_t(0x40000)}}) {
    auto image = Rom(0x100000, 0x20, uint8_t(type));
    image[0x7fd8] = uint8_t(ordinary);
    image[0x7fbd] = uint8_t(expansion);
    image[0x7fda] = 0x33;
    auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
    Check(cart && cart->BoardConnected() && cart->SramData().size() == bytes,
          "Ordinary RAM takes priority over expansion RAM and the graphics-family default");
  }
  auto image = Rom(0x100000, 0x20, 0xf5);
  image[0x7fbf] = 2;
  image[0x7fd8] = 3;
  image[0] = 0x78;
  image[0x123] = 0x45;
  const auto firmware = DetectionFirmware();
  auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, firmware.get());
  Check(cart && cart->Read(0x400000) == 0x78 && cart->Read(0xc00000) == 0x78 &&
            cart->Read(0x600123) == 0x45 && cart->Read(0xe00123) == 0x45,
        "The ARM cartridge retains its lower-half ROM windows");
}

void BootHeaderRanking() {
  auto image = Rom(0x100000, 0x21);
  const auto low = Rom();
  std::copy_n(low.begin() + 0x7fb0, 80, image.begin() + 0x7fb0);
  image[0] = 0xdb;
  const std::array<uint8_t, 8> program{0x78, 0xa9, 0x5a, 0x8f, 0x00, 0x10, 0x7e, 0xdb};
  std::copy(program.begin(), program.end(), image.begin() + 0x8000);
  auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().headerOffset == 0xffc0 &&
            cart->Header().mapping == MappingType::HiRom,
        "A plausible HiROM reset instruction outranks a stopped LoROM decoy");
  auto machine = Load(image);
  machine->GetBus().Write(0x7e1000, 0);
  machine->StepFrame();
  Check(machine->GetBus().Read(0x7e1000) == 0x5a,
        "The console executes the selected physical reset path");
  std::fill_n(image.begin() + 0xffdc, 4, 0);
  cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().headerOffset == 0xffc0,
        "A valid checksum cannot outweigh an implausible reset instruction");

  std::vector<uint8_t> extended(0x600000, 0xea);
  const auto high = Rom(0x500000, 0x25);
  std::copy_n(low.begin() + 0x7fb0, 80, extended.begin() + 0x407fb0);
  std::copy_n(high.begin() + 0x40ffb0, 80, extended.begin() + 0x40ffb0);
  extended[0x400000] = extended[0x408000] = 0x78;
  cart = Cartridge::FromRomImage(extended, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().headerOffset == 0x407fc0 &&
            cart->Header().mapping == MappingType::ExLoRom,
        "Equal extended reset candidates retain physical candidate priority");
}

void DeviceFamilyHeaders() {
  const auto firmware = DetectionFirmware();
  for (const auto [mode, type, chip] :
       {std::tuple{uint8_t(0x22), uint8_t(0x45), EnhancementChip::Sdd1},
        std::tuple{uint8_t(0x33), uint8_t(0x35), EnhancementChip::Sa1},
        std::tuple{uint8_t(0x30), uint8_t(0x16), EnhancementChip::SuperFx},
        std::tuple{uint8_t(0x20), uint8_t(0x4f), EnhancementChip::Sdd1},
        std::tuple{uint8_t(0x20), uint8_t(0x25), EnhancementChip::Obc1}}) {
    auto image = Rom(0x100000, mode, type);
    const auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
    Check(cart && cart->Header().chip == chip, "Device families retain both ROM-speed variants");
    Check(cart->Header().speed == ((mode & 0x10) ? RomSpeed::Fast : RomSpeed::Slow),
          "Enhancement headers retain the declared ROM speed");
  }
  auto conflicting = Rom(0x300000, 0x23, 0xf5);
  auto controller = Cartridge::FromRomImage(conflicting, nullptr, nullptr, nullptr);
  Check(controller && controller->Header().chip == EnhancementChip::Spc7110 &&
            controller->Header().mapping == MappingType::Spc7110,
        "A device family's controller takes priority over a conflicting mapping mode");
  const auto identified = [&](std::string_view title, uint8_t mode, uint8_t type, uint8_t subtype,
                              uint8_t maker) {
    auto image = Rom(0x100000, mode, type);
    std::fill_n(image.begin() + 0x7fc0, 21, ' ');
    std::copy(title.begin(), title.end(), image.begin() + 0x7fc0);
    image[0x7fbf] = subtype;
    image[0x7fda] = maker;
    return Cartridge::FromRomImage(image, nullptr, nullptr, nullptr, firmware.get());
  };
  auto dsp1 = identified("PILOTWINGS", 0x20, 5, 0, 0);
  auto dsp2 = identified("DUNGEON MASTER", 0x30, 5, 0, 0);
  auto dsp4 = identified("TOP GEAR 3000", 0x20, 3, 0, 0);
  Check(dsp1 && dsp1->Header().chip == EnhancementChip::Dsp1 && dsp2 &&
            dsp2->Header().chip == EnhancementChip::Dsp2 && dsp4 &&
            dsp4->Header().chip == EnhancementChip::Dsp4,
        "Firmware identity follows identifying title bytes across speed variants");
  auto padded = identified(std::string_view("DUNGEON\0 MASTER", 15), 0x30, 5, 0, 0);
  Check(padded && padded->Header().chip == EnhancementChip::Dsp2 &&
            padded->Header().title == "DUNGEON MASTER",
        "Embedded padding does not terminate the identifying title");
  auto japanese = identified("SD\xb6\xde\xdd\xc0\xde\xd1GX", 0x20, 5, 0, 0);
  Check(japanese && japanese->Header().chip == EnhancementChip::Dsp3 &&
            japanese->Header().title == "SD\xe3\x82\xac\xe3\x83\xb3\xe3\x83\x80\xe3\x83\xa0GX",
        "Halfwidth kana and voiced marks identify Japanese firmware titles");
  auto quizImage = Rom();
  const std::string_view quizTitle = "YUYU NO QUIZ DE GO!GO";
  std::copy(quizTitle.begin(), quizTitle.end(), quizImage.begin() + 0x7fc0);
  quizImage[0x7fd5] = 0x21;
  auto quiz = Cartridge::FromRomImage(quizImage, nullptr, nullptr, nullptr);
  Check(quiz && quiz->Header().mapping == MappingType::LoRom,
        "The extra title character does not select the wrong ROM layout");
  for (uint8_t mode : {0x22, 0x32, 0x23, 0x33}) {
    auto implicit = identified("T", mode, 0, 0, 0);
    Check(implicit && implicit->Header().chip ==
                          ((mode & 15) == 2 ? EnhancementChip::Sdd1 : EnhancementChip::Sa1),
          "A dedicated mapping mode selects its controller without a processor type");
  }
  for (uint8_t maker : {uint8_t(0), uint8_t(0x33), uint8_t(0xb2)}) {
    auto st011 = identified("2DAN MORITA SHOUGI", 0x30, 0xf5, 1, maker);
    auto cx4 = identified("T", 0x20, 0xf5, 0x10, maker);
    Check(st011 && st011->Header().chip == EnhancementChip::St011 && cx4 &&
              cx4->Header().chip == EnhancementChip::Cx4,
          "Extended subtype selection does not depend on the maker byte");
  }
}

void DetectionAndReload() {
  const auto firmware = DetectionFirmware();
  auto config = snes::tests::DeterministicConsoleConfig();
  config.cartridgeFirmware = firmware;
  struct Fixture {
    uint8_t mode, type, maker;
    EnhancementChip chip;
    bool supported;
  };
  for (const auto f : {Fixture{0x20, 3, 0, EnhancementChip::Dsp1, true},
                       Fixture{0x21, 5, 0, EnhancementChip::Dsp1, true},
                       Fixture{0x30, 5, 0, EnhancementChip::Dsp1, true},
                       Fixture{0x20, 5, 0, EnhancementChip::Dsp2, true},
                       Fixture{0x30, 5, 0xb2, EnhancementChip::Dsp3, true},
                       Fixture{0x30, 3, 0, EnhancementChip::Dsp4, true},
                       Fixture{0x30, 0x25, 0, EnhancementChip::Obc1, true},
                       Fixture{0x35, 0x55, 0, EnhancementChip::Srtc, true},
                       Fixture{0x23, 0x35, 0, EnhancementChip::Sa1, true},
                       Fixture{0x20, 0x15, 0, EnhancementChip::SuperFx, true},
                       Fixture{0x32, 0x43, 0, EnhancementChip::Sdd1, true},
                       Fixture{0x3a, 0xf5, 0, EnhancementChip::Spc7110, true},
                       Fixture{0x3a, 0xf9, 0, EnhancementChip::Spc7110Rtc, true},
                       Fixture{0x20, 0xf3, 0, EnhancementChip::Cx4, true},
                       Fixture{0x30, 0xf5, 0, EnhancementChip::St018, true},
                       Fixture{0x30, 0xf6, 0, EnhancementChip::St010, true}}) {
    auto bytes = Rom(f.mode == 0x35   ? 0x500000
                     : f.mode == 0x3a ? 0x300000
                                      : 0x100000,
                     f.mode, f.type);
    const size_t header = f.mode == 0x35 ? 0x40ffc0 : f.mode == 0x21 ? 0xffc0 : 0x7fc0;
    bytes[header + 0x1a] = f.maker;
    const std::string_view title = f.chip == EnhancementChip::Dsp2 ? "DUNGEON MASTER"
                                   : f.chip == EnhancementChip::Dsp3
                                       ? "SD\xb6\xde\xdd\xc0\xde\xd1GX"
                                   : f.chip == EnhancementChip::Dsp4 ? "TOP GEAR 3000"
                                                                     : "T";
    std::copy(title.begin(), title.end(), bytes.begin() + header);
    bytes[header - 1] = f.chip == EnhancementChip::Cx4     ? 0x10
                        : f.chip == EnhancementChip::St018 ? 2
                        : f.chip == EnhancementChip::St010 ? 1
                                                           : 0;
    auto cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr, firmware.get());
    Check(cart && cart->Header().chip == f.chip,
          "Chip detection uses device family, subtype and title");
    auto emu = std::make_unique<Emulator>(config);
    Check(emu->LoadCartridge(Rom()), "The reload fixture starts with an ordinary cartridge");
    emu->GetBus().Write(0x7e0123, 0x5a);
    std::string error;
    Check(emu->LoadCartridge(bytes, &error) == f.supported,
          "Load support matches the chip implementation");
    if (!f.supported) {
      Check(error.find(ChipName(f.chip)) != std::string::npos,
            "Unsupported chip error identifies the device");
      Check(emu->Initialized() && emu->GetBus().Read(0x7e0123) == 0x5a,
            "Failed load preserves the running machine");
      Check(emu->GetBus().Read(0x008000) == 0xea, "Failed load leaves valid ROM bus handlers");
      emu->StepFrame();
    }
  }
  auto st011 = Rom(0x100000, 0x30, 0xf6);
  st011[0x7fbf] = 1;
  std::copy_n("2DAN MORITA SHOUGI", 18, st011.begin() + 0x7fc0);
  Check(Cartridge::FromRomImage(st011, nullptr, nullptr, nullptr, firmware.get())->Header().chip ==
            EnhancementChip::St011,
        "ST011 detection uses its identifying title");
  auto emu = Load(Rom());
  emu->StepFrame();
  Check(emu->LoadCartridge(Rom()), "Reload succeeds");
  Check(emu->CurrentFrame() == 0 && emu->CurrentMasterCycles() == 0,
        "Reload resets frame counters");
}

void LoaderAndRam() {
  std::vector<uint8_t> blank(0x10000, 0);
  Check(Cartridge::FromRomImage(blank, nullptr, nullptr, nullptr).has_value(),
        "A blank full-bank image retains its fallback header");
  Check(!Cartridge::FromRomImage(std::span<const uint8_t>(blank).first(0x7fff), nullptr, nullptr,
                                 nullptr),
        "Images shorter than one complete bank have no header");
  std::fill(blank.begin(), blank.end(), 0xff);
  auto erased = Cartridge::FromRomImage(blank, nullptr, nullptr, nullptr);
  Check(erased && erased->Header().mapping == MappingType::LoRom && erased->Header().score == 0 &&
            erased->SramData().size() == 0x40000,
        "An erased full-bank image uses fallback metadata and bounded RAM");
  auto bytes = Rom();
  bytes[0x7fd8] = 3;
  auto cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
  CartridgeDatabase database;
  DatabaseOverride override;
  override.forceSramSize = 0;
  database.AddOverride(cart->RomCrc32(), override);
  cart = Cartridge::FromRomImage(bytes, nullptr, &database, nullptr);
  Check(cart->SramData().empty(), "A zero-byte SRAM override disables header SRAM");
  auto emu = Load(bytes);
  auto &bus = emu->GetBus();
  bus.Write(0x708123, 0x57);
  Check(bus.Read(0x700123) == 0x57 && bus.Read(0xf08123) == 0x57,
        "Small LoROM SRAM uses both bank halves");
  bytes[0x7fd8] = 6;
  emu = Load(bytes);
  emu->GetBus().Write(0x708123, 0x57);
  Check(emu->GetBus().Read(0x708123) == 0x57,
        "Small-ROM boards retain full-bank RAM with a larger RAM declaration");
  emu->GetBus().Write(0x700123, 0x57);
  emu->GetBus().Write(0x710123, 0x63);
  Check(emu->GetBus().Read(0x700123) == 0x57 && emu->GetBus().Read(0x710123) == 0x63,
        "Large SRAM banks remain independent");
  emu = Load(Rom());
  emu->GetBus().Write(0x7e0000, 0x63);
  Check(emu->GetBus().Read(0x700000) == 0x63, "Absent LoROM SRAM reads open bus");
  Check(emu->GetBus().Read(0x708000) == 0xea,
        "A board without RAM retains ROM in the upper half of bank 70");
  Check(emu->GetBus().Read(0xf08000) == 0xea, "Upper ROM mirror survives when SRAM is absent");
  emu = Load(Rom(0x100000, 0x21));
  emu->GetBus().Write(0x7e0000, 0x57);
  Check(emu->GetBus().Read(0x206000) == 0x57, "Absent HiROM SRAM reads open bus");
  emu->GetBus().Write(0x420d, 1);
  Check(emu->GetBus().Speed(0xc08000) == 6 && emu->LoadedCartridge()->AccessCycles(0xc08000) == 6,
        "MEMSEL timing does not depend on ROM header speed");

  // A 96 KiB image cannot be split into equal pairs of 32 KiB blocks.
  bytes = Rom(0x18000);
  bytes[0x7fd5] = 0x21;
  bytes.back() = 0x57;
  RomNormalizationInfo info;
  cart = Cartridge::FromRomImage(bytes, &info, nullptr, nullptr);
  Check(cart && !info.hadInterleave && cart->RomData().back() == 0x57,
        "Odd block count cannot drop the last bank");

  // Random low-bank bytes must not reorder a valid HiROM prototype.
  bytes = Rom(0x20000, 0x21);
  bytes[0x7fd5] = 0x21;
  bytes[0xffd5] = 0xff;
  bytes[0x8000] = 0x78;
  cart = Cartridge::FromRomImage(bytes, &info, nullptr, nullptr);
  Check(cart && !info.hadInterleave && cart->Read(0x008000) == 0x78,
        "Best header controls deinterleave detection");

  bytes = Rom(0x20000, 0x21);
  std::vector<uint8_t> interleaved(bytes.size());
  for (size_t pair = 0; pair < 2; ++pair) {
    std::copy_n(bytes.begin() + (pair * 2 + 1) * 0x8000, 0x8000,
                interleaved.begin() + pair * 0x8000);
    std::copy_n(bytes.begin() + pair * 0x10000, 0x8000, interleaved.begin() + (pair + 2) * 0x8000);
  }
  cart = Cartridge::FromRomImage(interleaved, &info, nullptr, nullptr);
  Check(cart && !info.hadInterleave &&
            std::equal(interleaved.begin(), interleaved.end(), cart->RomData().begin()),
        "A layout mismatch preserves the supplied bank order");
  bytes = Rom(0x500000, 0x35);
  bytes[0x40ffd5] = 0x21;
  bytes[0x408000] = 0x78;
  cart = Cartridge::FromRomImage(bytes, &info, nullptr, nullptr);
  Check(cart && cart->Header().mapping == MappingType::HiRom && cart->Read(0x008000) == 0xea &&
            cart->Read(0x408000) == 0x78,
        "A declared HiROM mode retains its wiring at an extended header position");
}

void InvalidMapperMetadata() {
  for (uint8_t mode : {0x00, 0x10, 0x40, 0xd0}) {
    auto bytes = Rom(0x20000, 0x21);
    bytes[0xffd5] = mode;
    bytes[0] = 0x42;
    bytes[0x8000] = 0x78;
    auto cart = Cartridge::FromRomImage(bytes, nullptr, nullptr, nullptr);
    Check(cart && cart->Header().mapping == MappingType::HiRom && cart->Read(0x008000) == 0x78,
          "Reserved mode bytes cannot override the physical reset header");
    Check(cart->Header().speed == RomSpeed::Slow, "Unknown speed metadata defaults to slow ROM");
  }
}

void RawBankOrder() {
  for (uint8_t mode : {0x20, 0x21, 0x25, 0x35}) {
    for (size_t size : {0x500000u, 0x600000u, 0x700000u, 0x800000u}) {
      auto bytes = Rom(size, mode);
      const size_t entry = mode == 0x20 ? 0 : mode == 0x21 ? 0x8000 : 0x408000;
      bytes[entry] = 0x78;
      for (size_t bank = 0; bank < size / 0x8000; ++bank)
        bytes[bank * 0x8000 + 0x1234] = uint8_t(bank);
      for (bool rotated : {false, true}) {
        auto raw = bytes;
        if (rotated)
          std::rotate(raw.begin(), raw.begin() + 0x400000, raw.end());
        for (bool copier : {false, true}) {
          auto input = raw;
          if (copier)
            input.insert(input.begin(), 512, 0);
          RomNormalizationInfo info;
          auto cart = Cartridge::FromRomImage(input, &info, nullptr, nullptr);
          Check(cart && !info.hadInterleave && info.hadCopierHeader == copier &&
                    cart->RomData().size() == raw.size() &&
                    std::equal(raw.begin(), raw.end(), cart->RomData().begin()),
                "Header hints cannot rotate or permute supplied ROM banks");
        }
      }
    }
  }
  auto tail = Rom(0x10600);
  tail[0] = 0x78;
  tail.back() = 0x63;
  RomNormalizationInfo info;
  auto cart = Cartridge::FromRomImage(tail, &info, nullptr, nullptr);
  Check(cart && !info.hadCopierHeader && cart->RomData().size() == tail.size() &&
            std::equal(tail.begin(), tail.end(), cart->RomData().begin()),
        "An ordinary partial-bank tail does not imply a copier prefix");

  auto large = Rom(0x600000);
  large[0] = 0x78;
  large[0x400000] = 0x22;
  cart = Cartridge::FromRomImage(large, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().mapping == MappingType::LoRom &&
            cart->Header().headerOffset == 0x7fc0 && cart->Read(0x808000) == 0x22,
        "Large ordinary LoROM retains its upper bank address line");
  large = Rom(0x600000, 0x21);
  large[0x8000] = 0x78;
  large[0x400000] = 0x33;
  cart = Cartridge::FromRomImage(large, nullptr, nullptr, nullptr);
  Check(cart && cart->Header().mapping == MappingType::HiRom &&
            cart->Header().headerOffset == 0xffc0 && cart->Read(0x400000) == 0x33,
        "Large ordinary HiROM retains its connected address lines");
}

void HeaderMemoryAndCountry() {
  for (const auto [declared, size] :
       {std::pair{0x00, 0u}, std::pair{0x03, 8192u}, std::pair{0xf3, 8192u},
        std::pair{0x09, 262144u}, std::pair{0x0f, 262144u}, std::pair{0xff, 262144u}}) {
    auto image = Rom(0x8000);
    image[0x7fd8] = uint8_t(declared);
    auto cart = Cartridge::FromRomImage(image, nullptr, nullptr, nullptr);
    Check(cart && cart->SramData().size() == size,
          "RAM declarations mask reserved bits and stop at the supported capacity");
  }
  auto image = Rom(0x8000);
  image[0x7fd8] = 9;
  const std::array<uint8_t, 22> program{0x78, 0xa9, 0x11, 0x8f, 0,    0,    0x70, 0xa9,
                                        0x22, 0x8f, 0,    0,    0x78, 0xaf, 0,    0,
                                        0x70, 0x8f, 0,    0x10, 0x7e, 0xdb};
  std::copy(program.begin(), program.end(), image.begin());
  auto machine = Load(image);
  machine->StepFrame();
  Check(machine->GetBus().Read(0x7e1000) == 0x22,
        "The CPU observes the capped RAM address-line alias");
  for (const auto [country, region] :
       {std::pair{0x00, Region::NTSC}, std::pair{0x01, Region::NTSC}, std::pair{0x0b, Region::NTSC},
        std::pair{0x0d, Region::NTSC}, std::pair{0x0f, Region::NTSC}, std::pair{0x10, Region::NTSC},
        std::pair{0x02, Region::PAL}, std::pair{0x0e, Region::PAL}, std::pair{0x11, Region::PAL},
        std::pair{0xff, Region::PAL}}) {
    image[0x7fd9] = uint8_t(country);
    Check(machine->LoadCartridge(image) && machine->GetTiming().GetRegion() == region,
          "Country metadata selects the same region for timing and video");
  }
}

void EnhancementHeaderSelection() {
  struct Board {
    uint8_t mode, type;
    EnhancementChip chip;
    MappingType mapping;
  };
  for (auto board : {Board{0x23, 0x35, EnhancementChip::Sa1, MappingType::Sa1},
                     Board{0x20, 0x15, EnhancementChip::SuperFx, MappingType::SuperFx},
                     Board{0x32, 0x45, EnhancementChip::Sdd1, MappingType::Sdd1},
                     Board{0x3a, 0xf5, EnhancementChip::Spc7110, MappingType::Spc7110},
                     Board{0x3a, 0xf9, EnhancementChip::Spc7110Rtc, MappingType::Spc7110}}) {
    auto bytes = Rom(0x600000, board.mode, board.type);
    const size_t header = board.mode == 0x3a ? 0xffc0 : 0x7fc0;
    if (header == 0xffc0) {
      std::copy_n(bytes.begin() + 0x7fc0, 64, bytes.begin() + header);
      std::fill_n(bytes.begin() + 0x7fc0, 64, 0);
    }
    bytes[header - 1] = 0;
    bytes[header + 0x18] = 3;
    // A stronger reset path outranks plausible metadata in data banks.
    bytes[header & ~size_t(0x7fff)] = 0x78;
    const auto ordinary = Rom(0x10000);
    for (size_t decoy : {0x407fc0u, 0x40ffc0u})
      std::copy_n(ordinary.begin() + 0x7fc0, 64, bytes.begin() + decoy);
    RomNormalizationInfo info;
    auto cart = Cartridge::FromRomImage(bytes, &info, nullptr, nullptr);
    Check(cart && cart->Header().chip == board.chip && cart->Header().mapping == board.mapping,
          "Enhancement board selection ignores ordinary extended-header decoys");
    Check(cart->Header().headerOffset == header && !info.hadInterleave &&
              std::equal(bytes.begin(), bytes.end(), cart->RomData().begin()),
          "Enhancement ROM data stays in place and the reset header supplies metadata");
  }
}

void BitmapProcessor() {
  auto image = Rom(0x100000, 0x20, 5);
  std::copy_n("DUNGEON MASTER", 14, image.begin() + 0x7fc0);
  auto emu = Load(image);
  auto &bus = emu->GetBus();
  const std::array<unsigned, 4> ports{0x308000, 0x3f8001, 0xb08fff, 0xbfbfff};
  for (unsigned port : ports)
    Check(bus.Read(port) == 0xff, "DSP-2 empty read");
  bus.Write(0x7e0000, 0x5a);
  Check(bus.Read(0x20c000) == 0x5a, "A no-RAM DSP-2 board leaves unused banks undriven");
  Check(bus.Read(0x30c000) == 0, "The mapped DSP-2 status window reads zero");
  bus.Write(ports[0], 9);
  for (unsigned i = 0; i < 4; ++i)
    bus.Write(ports[i], 0xff);
  const std::array<uint8_t, 4> product{1, 0, 0xfe, 0xff};
  for (unsigned i = 0; i < 4; ++i)
    Check(bus.Read(ports[i]) == product[i], "DSP-2 unsigned multiplication");
  Check(bus.Read(ports[0]) == 0xff, "DSP-2 output consumption");
  bus.Write(ports[0], 1);
  for (unsigned row = 0; row < 8; ++row)
    for (uint8_t value : {0x01, 0x23, 0x45, 0x67})
      bus.Write(ports[row % 4], value);
  for (unsigned i = 0; i < 32; ++i)
    Check(bus.Read(ports[i % 4]) == (i < 16 ? (i % 2 ? 0x33 : 0x55) : (i % 2 ? 0 : 0x0f)),
          "DSP-2 packed pixels become four bitplanes");
  for (uint8_t byte : {3, 0x1f, 5, 2, 0x12, 0x34, 0xf5, 0x6f})
    bus.Write(ports[0], byte);
  Check(bus.Read(ports[1]) == 0x15 && bus.Read(ports[2]) == 0x64, "DSP-2 transparent overlay");
  for (uint8_t byte : {6, 3, 0x12, 0x34, 0x56})
    bus.Write(ports[0], byte);
  for (uint8_t byte : {0x65, 0x43, 0x21})
    Check(bus.Read(ports[3]) == byte, "DSP-2 reverse pixels");
  // Fixed-point shrinking samples source pixels at 0, 3, 6, 9.
  for (uint8_t byte : {0x0d, 8, 2, 0x12, 0x34, 0x56, 0x78})
    bus.Write(ports[0], byte);
  Check(bus.Read(ports[0]) == 0x14 && bus.Read(ports[0]) == 0x71,
        "DSP-2 fixed-point scale and retained parameter RAM");
  for (uint8_t op : {5, 6, 0x0d}) {
    bus.Write(ports[0], op);
    bus.Write(ports[0], 0);
    if (op == 0x0d)
      bus.Write(ports[0], 0);
    for (uint8_t byte : {9, 2, 0, 3, 0})
      bus.Write(ports[0], byte);
    Check(bus.Read(ports[0]) == 6,
          "A zero-length DSP-2 bitmap request still permits a different command");
    for (unsigned i = 0; i < 3; ++i)
      Check(bus.Read(ports[0]) == 0, "DSP-2 result after an empty request");
  }
  bus.Write(ports[0], 5);
  bus.Write(ports[0], 0);
  Check(bus.Read(ports[0]) == 0xff,
        "Repeating the zero-length command clears its retained payload phase");
  bus.Write(ports[0], 5);
  bus.Write(ports[0], 255);
  for (unsigned i = 0; i < 255; ++i)
    bus.Write(ports[0], 0xab);
  for (unsigned i = 0; i < 255; ++i)
    bus.Write(ports[0], 0xff);
  for (unsigned i = 0; i < 255; ++i)
    Check(bus.Read(ports[0]) == 0xab, "DSP-2 maximum overlay length");
  Check(emu->LoadCartridge(Rom()), "Replacing a DSP-2 cartridge succeeds");
  Check(emu->GetBus().Read(0x208000) == 0xea, "Reload removes DSP-2 handlers");
}

void ObjectController() {
  auto image = Rom(0x100000, 0x30, 0x25);
  image[0x7fd8] = 3;
  auto emu = Load(image);
  auto &bus = emu->GetBus();
  Check(bus.Read(0x006000) == 0xff && bus.Read(0xbf7ff6) == 0xff, "OBC1 power-on RAM");
  for (unsigned byte = 0; byte < 4; ++byte)
    bus.Write(0x7e7ff0 + byte, uint8_t(0xf0 + byte));
  for (unsigned select = 0; select < 2; ++select) {
    bus.Write(0x007ff5, static_cast<uint8_t>(select));
    const unsigned base = select ? 0x7800 : 0x7c00;
    for (unsigned object : {0u, 1u, 126u, 127u}) {
      bus.Write(0x807ff6, static_cast<uint8_t>(object | 0x80));
      for (unsigned byte = 0; byte < 4; ++byte) {
        bus.Write(0x3f7ff0 + byte, static_cast<uint8_t>(object + byte));
        Check(bus.Read(base + object * 4 + byte) == uint8_t(object + byte),
              "OBC1 object port writes backing RAM");
        Check(bus.Read(0xbf7ff0 + byte) == uint8_t(object + byte),
              "OBC1 object ports share bank mirrors");
        Check(bus.Read(0x7e7ff0 + byte) == uint8_t(0xf0 + byte), "OBC1 leaves WRAM mapped");
      }
    }
    bus.Write(base + 0x200, 0);
    for (unsigned object = 0; object < 4; ++object) {
      bus.Write(0x007ff6, static_cast<uint8_t>(object));
      bus.Write(0x007ff4, static_cast<uint8_t>(object | 0xfc));
    }
    Check(bus.Read(0x007ff4) == 0xe4 && bus.Read(base + 0x200) == 0xe4,
          "OBC1 two-bit attributes preserve neighbors");
  }
  Check(bus.Read(0x007ff5) == 1 && bus.Read(0x807ff6) == 3, "OBC1 selector readback");
  Check(emu->LoadCartridge(image), "OBC1 reload");
  Check(emu->GetBus().Read(0x007ff6) == 0xff && emu->GetBus().Read(0x007800) == 0xff,
        "OBC1 resets on reload");
}

void Signature(std::vector<uint8_t> &bytes, size_t offset, std::string_view text) {
  std::copy(text.begin(), text.end(), bytes.begin() + offset);
}

void Sufami() {
  auto bios = Rom(0x40000);
  Signature(bios, 0, "BANDAI SFC-ADX");
  Signature(bios, 0x10, "SFC-ADX BACKUP");
  std::vector<uint8_t> a(0x80000, 0x11), b(0x100000, 0x22);
  Signature(a, 0, "BANDAI SFC-ADX");
  Signature(b, 0, "BANDAI SFC-ADX");
  a[0x37] = 4;
  b[0x37] = 1;
  auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
  Check(emu->LoadSufamiTurbo(bios, a, b), "Load two Sufami Turbo slots");
  Check(emu->LoadedCartridge()->Header().mapping == MappingType::SufamiTurbo,
        "Sufami Turbo mapper selection");
  auto &bus = emu->GetBus();
  for (unsigned bank : {0u, 7u, 8u, 0x1fu, 0x80u, 0x9fu})
    Check(bus.Read((bank << 16) | 0x8123) == bios[((bank & 7) << 15) | 0x123],
          "Sufami BIOS bank mirroring");
  for (unsigned bank : {0x20u, 0x2fu, 0x30u, 0x3fu, 0xa0u, 0xbfu})
    Check(bus.Read((bank << 16) | 0x8123) == 0x11, "Sufami slot A mirrors its own ROM");
  for (unsigned bank : {0x40u, 0x5fu, 0xc0u, 0xdfu})
    Check(bus.Read((bank << 16) | 0x8123) == 0x22, "Sufami slot B bank selection");
  bus.Write(0x7e0000, 0xa5);
  Check(bus.Read(0x400123) == 0x22 && bus.Read(0xdf0123) == 0x22,
        "Sufami slot B ROM occupies both bank halves");
  bus.Write(0x608123, 0x57);
  bus.Write(0x708123, 0x63);
  Check(bus.Read(0xe3c123) == 0x57 && bus.Read(0xf3c123) == 0x63,
        "Sufami slot SRAM is separate and mirrored");
  Check(emu->LoadedCartridge()->SlotSramData(0)[0x123] == 0x57 &&
            emu->LoadedCartridge()->SlotSramData(1)[0x123] == 0x63,
        "Sufami save API exposes both slots");
  Check(bus.Read(0x600123) == 0x57 && bus.Read(0x6fe123) == 0x57 && bus.Read(0xef0123) == 0x57 &&
            bus.Read(0x700123) == 0x63 && bus.Read(0x7d8923) == 0x63 && bus.Read(0xff0123) == 0x63,
        "Sufami RAM occupies full banks and mirrors each slot's capacity");
  bus.Write(0x6f2123, 0x48);
  bus.Write(0xff0923, 0x79);
  Check(bus.Read(0x608123) == 0x48 && bus.Read(0x708123) == 0x79,
        "Sufami RAM writes use lower halves and outer bank aliases");
  bus.Write(0x608123, 0x57);
  bus.Write(0x708123, 0x63);
  const std::vector<uint8_t> saved(emu->LoadedCartridge()->SlotSramData(0).begin(),
                                   emu->LoadedCartridge()->SlotSramData(0).end());
  bus.Write(0x7e8123, 0xa5);
  Check(bus.Read(0x7e8123) == 0xa5, "Sufami preserves WRAM");
  bus.Write(0x400123, 0x76);
  Check(bus.Read(0x400123) == 0x22, "Sufami ROM ignores writes in its lower bank half");
  Check(!emu->LoadSufamiTurbo(bios, bios, b), "Reject a BIOS passed as a game");
  Check(bus.Read(0x608123) == 0x57, "Failed Sufami load preserves the existing slots");
  bios.insert(bios.begin(), 512, 0);
  Check(emu->LoadSufamiTurbo(bios, {}, b),
        "Sufami supports an empty first slot and copier headers");
  bus.Write(0x7e0000, 0x42);
  Check(bus.Read(0x208123) == 0x42 && bus.Read(0x608123) == 0x42,
        "Absent Sufami slot reads open bus");
  Check(emu->LoadedCartridge()->SlotSramData(0).empty(), "Absent Sufami slot has no save");
  emu->LoadSlotSram(1, saved);
  Check(bus.Read(0xf08123) == 0x57, "A Sufami save can follow its game to the other slot");
  Check(emu->LoadSufamiTurbo(bios, {}, {}), "Sufami BIOS-only operation");
  a[0x37] = 0;
  Check(emu->LoadSufamiTurbo(bios, a, a), "Sufami games can omit RAM");
  bus.Write(0x7e0000, 0xa5);
  Check(bus.Read(0x600123) == 0xa5 && bus.Read(0xff8123) == 0xa5 &&
            emu->LoadedCartridge()->SramData().empty() && emu->SaveSlotBattery(0).empty(),
        "RAM-free Sufami slots leave their RAM windows open");
  Check(!emu->LoadSlotBattery(0, saved), "A RAM-free slot has no battery storage to restore");
  a.resize(0x300000, 0x11);
  std::fill(a.begin() + 0x100000, a.begin() + 0x200000, 0x52);
  std::fill(a.begin() + 0x200000, a.end(), 0x73);
  a[0x37] = 3;
  Check(emu->LoadSufamiTurbo(bios, a, a), "Sufami accepts larger slot images");
  Check(bus.Read(0x208123) == 0x52 && bus.Read(0xa08123) == 0x52 && bus.Read(0x400123) == 0x73 &&
            bus.Read(0xc00123) == 0x73,
        "Sufami ROM mirroring retains the slot's bank address lines");
  bus.Write(0x601123, 0x49);
  bus.Write(0x600923, 0x61);
  Check(bus.Read(0xef1923) == 0x49 && bus.Read(0x608923) == 0x61,
        "Sufami non-power-of-two RAM mirrors its populated blocks");
  Check(emu->Reset() && bus.Read(0x601923) == 0x49 && bus.Read(0xe08923) == 0x61,
        "Sufami RAM retains writes through console reset");
}

std::array<uint8_t, 13> ReadClock(Srtc &clock) {
  clock.Write(13);
  Check(clock.Read() == 15, "S-RTC leading marker");
  std::array<uint8_t, 13> digits;
  for (auto &digit : digits)
    digit = clock.Read();
  Check(clock.Read() == 15, "S-RTC trailing marker");
  return digits;
}

void Clock() {
  int64_t now = 1700000000;
  Srtc clock([&] { return now; });
  const auto program = [&](std::array<uint8_t, 12> digits) {
    clock.Write(14);
    clock.Write(0);
    for (uint8_t digit : digits)
      clock.Write(digit);
  };
  program({9, 5, 9, 5, 3, 2, 8, 2, 2, 4, 2, 10}); // 2024-02-28 23:59:59
  auto digits = ReadClock(clock);
  Check(digits[12] == 3, "S-RTC computes weekday when programmed");
  now += 2;
  Check(ReadClock(clock) == digits, "Host time does not advance a running cartridge calendar");
  clock.Write(13);
  Check(clock.Read() == 15 && clock.Read() == 9, "S-RTC starts a live calendar read");
  clock.Advance(1);
  clock.Save();
  Check(clock.Read() == 0,
        "Reads observe a calendar edge without a save changing the serial cursor");
  clock.Advance(21'477'272);
  digits = ReadClock(clock);
  Check(digits[0] == 1 && digits[1] == 0 && digits[4] == 0 && digits[6] == 9 && digits[7] == 2 &&
            digits[12] == 3,
        "S-RTC leap-day rollover retains its programmed weekday register");
  const auto saved = clock.Save();
  now += 86400;
  Srtc restored([&] { return now; });
  Check(restored.Load(saved), "S-RTC save round trip");
  digits = ReadClock(restored);
  Check(digits[6] == 1 && digits[7] == 0 && digits[8] == 3 && digits[12] == 3,
        "S-RTC advances while the emulator is closed");
  now -= 3600;
  Check(ReadClock(restored) == digits, "S-RTC ignores host clock rollback");
  auto corrupt = saved;
  corrupt[13] = 0;
  Check(!restored.Load(corrupt), "S-RTC rejects an invalid save signature");
  Check(!restored.Load(std::span(saved).first(20)), "S-RTC rejects truncated saves");
  program({9, 5, 9, 5, 3, 2, 8, 2, 2, 0, 0, 11}); // 2100 is not a leap year.
  clock.Reset();
  clock.Advance(1);
  digits = ReadClock(clock);
  Check(digits[6] == 1 && digits[7] == 0 && digits[8] == 3, "S-RTC century rollover");
  clock.Write(14);
  clock.Write(4);
  Check(clock.Read() == 0, "S-RTC clear leaves read mode");
  digits = ReadClock(clock);
  Check(std::all_of(digits.begin(), digits.end(), [](uint8_t d) { return d == 0; }),
        "S-RTC clear command");
  auto image = Rom(0x500000, 0x35, 0x55);
  image[0x40ffd8] = 3;
  auto emu = Load(image);
  auto &bus = emu->GetBus();
  bus.Write(0xbf2801, 0x1e);
  bus.Write(0x002801, 4);
  bus.Write(0x802801, 13);
  Check(bus.Read(0x3f2800) == 15 && bus.Read(0x802800) == 0,
        "S-RTC ports share mirrors and mask command nibbles");
  bus.Write(0x7e0000, 0x57);
  Check(bus.Read(0x002801) == 0x57, "S-RTC write port reads open bus");
  Check(emu->SaveRtc().size() == Srtc::SaveSize && emu->LoadRtc(saved),
        "Emulator exposes cartridge clock persistence");
}

void Regions() {
  for (const auto [country, pal] :
       {std::pair{0x00, false}, std::pair{0x01, false}, std::pair{0x02, true},
        std::pair{0x0b, false}, std::pair{0x0c, true}, std::pair{0x0d, false},
        std::pair{0x11, true}, std::pair{0x12, true}, std::pair{0xff, true}}) {
    auto bytes = Rom();
    bytes[0] = 0x80;
    bytes[1] = 0xfe; // BRA to itself.
    bytes[0x7fd9] = static_cast<uint8_t>(country);
    auto emu = Load(bytes);
    Check(emu->GetTiming().GetRegion() == (pal ? Region::PAL : Region::NTSC),
          "Header selects the television region");
    Check((emu->GetBus().Read(0x213f) & 0x10) == (pal ? 0x10 : 0), "PPU reports television region");
    Check(emu->GetTiming().VPeriod() == (pal ? 312 : 262), "Region selects the scanline count");
    const auto first = emu->StepFrame();
    const auto frame = emu->StepFrame();
    const uint64_t target = pal ? 425568 : 357368;
    Check(frame.masterCycles - first.masterCycles == target,
          "Frame execution uses the selected region");
    const uint64_t apuTarget =
        frame.masterCycles * Smp::kClockFrequency / (pal ? 21281370 : 21477272);
    Check(emu->GetSmp().CycleCount() >= apuTarget && emu->GetSmp().CycleCount() < apuTarget + 20,
          "APU synchronization uses the region clock");
  }
}

void CombinedSufami() {
  auto bios = Rom(0x40000);
  const std::string_view signature = "BANDAI SFC-ADX";
  const std::string_view backup = "SFC-ADX BACKUP";
  std::copy(signature.begin(), signature.end(), bios.begin());
  std::copy(backup.begin(), backup.end(), bios.begin() + 0x10);
  std::vector<uint8_t> combined(0x300000, 0xff);
  std::copy(bios.begin(), bios.end(), combined.begin());
  for (const auto offset : {0x100000u, 0x200000u}) {
    std::fill_n(combined.begin() + offset, 0x100000, offset == 0x100000 ? 0x31 : 0x72);
    std::copy(signature.begin(), signature.end(), combined.begin() + offset);
    combined[offset + 0x37] = offset == 0x100000 ? 4 : 1;
  }
  auto emu = Load(combined);
  Check(emu->LoadedCartridge()->Header().mapping == MappingType::SufamiTurbo,
        "Combined image selects Sufami mapping");
  auto &bus = emu->GetBus();
  Check(bus.Read(0x208100) == 0x31 && bus.Read(0x408100) == 0x72,
        "Combined image maps both game slots");
  bus.Write(0x608010, 0x19);
  bus.Write(0x708010, 0x83);
  const auto save = emu->LoadedCartridge()->SramData();
  const std::vector<uint8_t> saved(save.begin(), save.end());
  Check(saved.size() == 0x2800 && saved[0x10] == 0x19 && saved[0x2010] == 0x83,
        "Combined saves contain two independent SRAM regions");
  combined.insert(combined.begin(), 512, 0);
  RomNormalizationInfo info;
  auto cart = Cartridge::FromRomImage(combined, &info, nullptr, nullptr);
  Check(cart && info.hadCopierHeader && !info.hadInterleave,
        "Combined copier header normalization");
  Check(emu->LoadCartridge(combined), "Reload combined cartridge");
  emu->LoadSram(saved);
  Check(bus.Read(0xe08010) == 0x19 && bus.Read(0xf08010) == 0x83, "Combined save round trip");
  combined.erase(combined.begin(), combined.begin() + 512);
  std::fill(combined.begin() + 0x100000, combined.begin() + 0x200000, 0xff);
  Check(emu->LoadCartridge(combined), "Combined image permits an erased slot");
  bus.Write(0x7e0000, 0x53);
  Check(bus.Read(0x208100) == 0x53 && bus.Read(0x608100) == 0x53,
        "Empty combined slot is open bus");
  combined.resize(0x280000);
  Check(!emu->LoadCartridge(combined), "Reject truncated combined slot");
  Check(bus.Read(0x408100) == 0x72, "Failed combined load preserves cartridge");
  Check(emu->LoadCartridge(bios), "Standalone base image loads");
  bus.Write(0x7e0000, 0x91);
  Check(bus.Read(0x208000) == 0x91, "Standalone base image has no game ROM in slot A");
}

void BroadcastBoards() {
  for (const bool hi : {false, true}) {
    auto base = Rom(0x200000, hi ? 0x21 : 0x20);
    const unsigned header = hi ? 0xffc0 : 0x7fc0;
    std::copy_n("ZGNJ", 4, base.begin() + header - 14);
    base[header + 0x1a] = 0x33;
    base[header + 0x18] = 3;
    base[0x8000] = 0x4c;
    base[0x8001] = 0;
    base[0x8002] = 0x80;
    base[0x8123] = 0x36;
    std::vector<uint8_t> pack(0x100000, 0xff);
    pack[0x8123] = 0x53;
    pack[0x18123] = 0x72;
    auto emu = std::make_unique<Emulator>(snes::tests::DeterministicConsoleConfig());
    std::string error;
    Check(emu->LoadBroadcastCartridge(base, pack, &error), "BS base and memory pack load");
    auto &bus = emu->GetBus();
    Check(emu->LoadedCartridge()->Header().mapping ==
              (hi ? MappingType::BroadcastHiRom : MappingType::BroadcastLoRom),
          "BS board selects the correct mapper");
    Check(bus.Read(hi ? 0x008123 : 0x018123) == 0x36, "BS base ROM read");
    const unsigned port = hi ? 0xe08123 : 0xc08123;
    Check(bus.Read(port) == 0x53, "BS memory pack ROM read");
    Check(bus.Read(hi ? 0xf08123 : 0xe08123) == 0x53, "BS memory pack bank mirror");
    const unsigned ram = hi ? 0x206123 : 0x700123;
    bus.Write(ram, 0xa9);
    Check(bus.Read(ram) == 0xa9, "BS base SRAM remains writable");
    bus.Write(0x7e8123, 0x94);
    Check(bus.Read(0x7e8123) == 0x94, "BS mapping preserves WRAM");
    if (hi) {
      for (unsigned address : {0x208123u, 0x608123u, 0xa08123u})
        Check(bus.Read(address) == 0x53, "HiROM pack data is visible through each connected alias");
      bus.Write(0x208123, 0x40);
      bus.Write(0x208123, 0);
      Check(bus.Read(port) == 0xb0 && emu->LoadedCartridge()->MemoryPackData()[0x8123] == 0x53,
            "HiROM aliases access the same protected flash command port");
      bus.Write(port, 0x50);
      bus.Write(port, 0xff);
    } else {
      bus.Write(0x7e0000, 0x62);
      Check(bus.Read(0x408000) == 0x62 && bus.Read(0x406123) == 0x62 &&
                bus.Read(0x708123) == 0x62 && bus.Read(0xf08123) == 0x62,
            "BS LoROM leaves unused full banks and the upper SRAM halves open");
    }
    bus.Write(port, 0x40);
    bus.Write(port, 0x0f);
    Check(bus.Read(port) == 0xb0 && bus.Read(port) == 0xb0 &&
              emu->LoadedCartridge()->MemoryPackData()[0x8123] == 0x53,
          "A slot's write-protect input rejects writes to locked blocks");
    auto metadata = emu->LoadedCartridge()->MemoryPackMetadata();
    for (unsigned block = 0; block < 16; ++block)
      metadata[20 + block * 5] = 0;
    Check(emu->LoadMemoryPackMetadata(metadata), "Downloaded block-lock metadata restores");
    bus.Write(port, 0x97);
    bus.Write(port, 0xd0);
    bus.Write(port, 0x50);
    bus.Write(port, 0x40);
    bus.Write(port, 0x0f);
    Check(bus.Read(port) == 0x80 && bus.Read(port) == 0x80,
          "An unlocked slot block programs and retains compatible status");
    bus.Write(port, 0xff);
    Check(bus.Read(port) == 3, "Array mode exposes the programmed flash byte");
    bus.Write(port, 0x10);
    bus.Write(port, 0xff);
    bus.Write(port, 0xff);
    Check(bus.Read(port) == 3, "Flash programming cannot set cleared bits");
    bus.Write(port, 0x71);
    const unsigned regBank = hi ? 0xe00000 : 0xc00000;
    Check(bus.Read(regBank + 2) == 0xc0 && bus.Read(regBank + 4) == 0x86 &&
              bus.Read(regBank + 0x8004) == 0,
          "Flash block and global status have distinct physical offsets");
    bus.Write(port, 0x90);
    Check(bus.Read(regBank) == 0xb0 && bus.Read(regBank + 1) == 0xa8,
          "Chip identifiers report the connected flash capacity");
    bus.Write(port, 0xff);
    bus.Write(port, 0x20);
    bus.Write(port, 0xd0);
    Check(bus.Read(port) == 0, "Block erase reports busy before its delay completes");
    for (unsigned field = 0; field < 20; ++field)
      emu->StepFrame();
    bus.Write(port, 0xff);
    Check(bus.Read(port) == 0xff, "Console clocks complete the flash block erase");
    Check(bus.Read(hi ? 0xe18123 : 0xc18123) == 0x72, "Block erase preserves the adjacent block");
    bus.Write(port, 0x40);
    bus.Write(port, 0x64);
    bus.Write(port, 0xff);
    const auto savedMetadata = emu->LoadedCartridge()->MemoryPackMetadata();
    const auto data = emu->LoadedCartridge()->MemoryPackData();
    const std::vector<uint8_t> saved(data.begin(), data.end());
    Check(!emu->LoadMemoryPack(std::span(saved).first(50)), "Reject short memory-pack save");
    Check(bus.Read(port) == 0x64, "Invalid save leaves memory pack intact");
    const std::vector<uint8_t> blankPack(saved.size(), 0xff);
    Check(emu->LoadBroadcastCartridge(base, blankPack, &error), "Supplied blank memory pack loads");
    Check(emu->LoadMemoryPack(saved) && bus.Read(port) == 0x64, "Memory pack save round trip");
    Check(emu->LoadMemoryPackMetadata(savedMetadata) &&
              emu->LoadedCartridge()->MemoryPackMetadata() == savedMetadata,
          "Flash lock state and erase counts restore beside the array save");
    base[header + 0x1a] = 0;
    Check(!emu->LoadBroadcastCartridge(base, pack, &error),
          "Reject a base without slot identification");
    Check(bus.Read(port) == 0x64, "Rejected base leaves the loaded pack intact");
    base[header + 0x1a] = 0x33;
    Check(!emu->LoadBroadcastCartridge(base, std::span(pack).first(0x80000), &error),
          "Reject unsupported pack size");
    pack[0xff00] = 'M';
    pack[0xff02] = 'P';
    pack[0xff06] = 0x70;
    Check(emu->LoadBroadcastCartridge(base, pack, &error), "Read-only mask ROM pack loads");
    bus.Write(port, 0x40);
    bus.Write(port, 0);
    Check(bus.Read(port) == 0x53 && emu->LoadedCartridge()->MemoryPackData().empty(),
          "Mask ROM pack ignores writes and has no flash save");
  }
  auto base = Rom(0x300000);
  const std::string_view title = "SOUND NOVEL-TCOOL";
  std::fill_n(base.begin() + 0x7fc0, 21, ' ');
  std::copy(title.begin(), title.end(), base.begin() + 0x7fc0);
  std::copy_n("ZSTJ", 4, base.begin() + 0x7fb2);
  base[0x7fd8] = 3;
  base[0x7fda] = 0x33;
  base[0x123] = 0x11;
  base[0x100123] = 0x22;
  base[0x200123] = 0x33;
  auto cart = Cartridge::FromBroadcastCartridge(base, {});
  Check(cart && cart->Read(0x008123) == 0x11 && cart->Read(0x208123) == 0x22 &&
            cart->Read(0x808123) == 0x33 && cart->Read(0xa08123) == 0x22,
        "BS 24-Mbit board selects each base ROM chip");
}
} // namespace

int main() {
  try {
    IdentifiedSlotCartridges();
    CompleteBoardProfiles();
    HeaderRamDeclarations();
    BootHeaderRanking();
    DeviceFamilyHeaders();
    DetectionAndReload();
    LoaderAndRam();
    BitmapProcessor();
    ObjectController();
    Sufami();
    Clock();
    Regions();
    CombinedSufami();
    BroadcastBoards();
    InvalidMapperMetadata();
    EnhancementHeaderSelection();
    RawBankOrder();
    HeaderMemoryAndCountry();
    std::printf("%u cartridge device checks passed\n", checks);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
    return 1;
  }
}
