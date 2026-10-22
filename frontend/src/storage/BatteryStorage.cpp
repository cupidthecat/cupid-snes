#include "BatteryStorage.hpp"
#include "snes/core/FilePath.hpp"

#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace snes::frontend {
namespace {
std::filesystem::path Sidecar(const std::string &source, const char *extension,
                              const std::filesystem::path &directory, bool canonical = false) {
  auto path = core::FilePath(source);
  if (canonical)
    path = std::filesystem::weakly_canonical(path);
  path.replace_extension(extension);
  return directory.empty() ? path : directory / path.filename();
}
bool SameSavePath(const std::filesystem::path &first, const std::filesystem::path &second) {
  if (first.empty() || second.empty())
    return false;
  if (first == second)
    return true;
  std::error_code error;
  if (std::filesystem::equivalent(first, second, error))
    return true;
#ifdef _WIN32
  const auto &left = first.native(), &right = second.native();
  return CompareStringOrdinal(left.c_str(), int(left.size()), right.c_str(), int(right.size()),
                              TRUE) == CSTR_EQUAL;
#else
  return false;
#endif
}
} // namespace

BatteryStorage::BatteryStorage(const LaunchRequest &request, core::Emulator &machine,
                               const std::filesystem::path &directory)
    : kind_(request.kind) {
  std::array<std::filesystem::path, Count> paths;
  std::array<size_t, Count> sizes{};
  const auto &source = request.paths;
  const auto *cart = machine.LoadedCartridge();
  if (kind_ == LaunchRequest::Kind::Handheld) {
    paths[Ram] = Sidecar(source[1], ".sav", directory, true);
    sizes[Ram] = machine.SaveSlotBattery(0).size();
  } else if (kind_ == LaunchRequest::Kind::SufamiTurbo) {
    for (size_t slot = 0; slot < 2; ++slot) {
      sizes[SlotA + slot] = cart->SlotSramData(slot).size();
      if (sizes[SlotA + slot])
        paths[SlotA + slot] = Sidecar(source[slot + 1], ".srm", directory, true);
    }
  } else {
    paths[Ram] = Sidecar(source[0], ".srm", directory);
    paths[Clock] = Sidecar(source[0], ".rtc", directory);
    auto pack = core::FilePath(kind_ == LaunchRequest::Kind::Broadcast && source[1] != "-" &&
                                       !source[1].empty()
                                   ? source[1]
                                   : source[0]);
    pack += ".flash";
    paths[Pack] = directory.empty() ? pack : directory / pack.filename();
    paths[PackMetadata] = paths[Pack];
    paths[PackMetadata] += ".meta";
    sizes[Ram] = cart->SramData().size();
    sizes[Clock] = machine.SaveRtc().size();
    sizes[Pack] = cart->MemoryPackData().size();
    sizes[PackMetadata] = cart->MemoryPackMetadata().size();
  }
  for (size_t index = 0; index < paths.size(); ++index) {
    if (sizes[index]) {
      if (std::filesystem::exists(paths[index]) && !std::filesystem::is_regular_file(paths[index]))
        throw std::runtime_error("Save path must be a file: " + core::PathText(paths[index]));
      for (size_t prior = 0; prior < index; ++prior)
        if (sizes[prior] && SameSavePath(paths[index], paths[prior]))
          throw std::runtime_error("Cartridge storage would share a save file: " +
                                   core::PathText(paths[index]));
    }
    if (!paths[index].empty())
      files_[index] = std::make_unique<SaveRamFile>(paths[index]);
  }
}

void BatteryStorage::Load(core::Emulator &machine) {
  const auto *cart = machine.LoadedCartridge();
  if (kind_ == LaunchRequest::Kind::Handheld) {
    const auto bytes = files_[Ram]->LoadUpTo(machine.SaveSlotBattery(0).size());
    if (!bytes.empty() && !machine.LoadSlotBattery(0, bytes))
      throw std::runtime_error("Invalid handheld battery save");
  } else if (kind_ == LaunchRequest::Kind::SufamiTurbo) {
    for (size_t slot = 0; slot < 2; ++slot)
      if (files_[SlotA + slot])
        machine.LoadSlotSram(slot, files_[SlotA + slot]->Load(cart->SlotSramData(slot).size()));
  } else {
    machine.LoadSram(files_[Ram]->Load(cart->SramLoadSize()));
    const auto clock = files_[Clock]->Load(machine.SaveRtc().size());
    if (!clock.empty() && !machine.LoadRtc(clock))
      throw std::runtime_error("Invalid cartridge clock save: " +
                               core::PathText(files_[Clock]->Path()));
    const auto pack = files_[Pack]->Load(cart->MemoryPackData().size());
    if (!pack.empty() && !machine.LoadMemoryPack(pack))
      throw std::runtime_error("Invalid memory pack save: " + core::PathText(files_[Pack]->Path()));
    const auto metadata = files_[PackMetadata]->Load(cart->MemoryPackMetadata().size());
    if (!metadata.empty() && !machine.LoadMemoryPackMetadata(metadata))
      throw std::runtime_error("Invalid memory pack metadata: " +
                               core::PathText(files_[PackMetadata]->Path()));
  }
}

void BatteryStorage::Flush(core::Emulator &machine) {
  const auto *cart = machine.LoadedCartridge();
  if (kind_ == LaunchRequest::Kind::Handheld)
    files_[Ram]->Flush(machine.SaveSlotBattery(0));
  else if (kind_ == LaunchRequest::Kind::SufamiTurbo) {
    for (size_t slot = 0; slot < 2; ++slot)
      if (files_[SlotA + slot])
        files_[SlotA + slot]->Flush(cart->SlotSramData(slot));
  } else {
    files_[Ram]->Flush(cart->SramData());
    files_[Clock]->Flush(machine.SaveRtc());
    files_[Pack]->Flush(cart->MemoryPackData());
    files_[PackMetadata]->Flush(cart->MemoryPackMetadata());
  }
}
} // namespace snes::frontend
