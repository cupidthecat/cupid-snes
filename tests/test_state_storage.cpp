#include "ConsoleConfig.hpp"
#include "session/Session.hpp"
#include "snes/core/FilePath.hpp"
#include "storage/AtomicFile.hpp"
#include "storage/StateStorage.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace snes::frontend;
namespace {
void Check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class Operation> void Reject(Operation operation, const char *message) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::exception &) {
    rejected = true;
  }
  Check(rejected, message);
}
template <class Operation> void Cancelled(Operation operation) {
  bool cancelled = false;
  try {
    operation();
  } catch (const snes::core::StateCancelled &) {
    cancelled = true;
  }
  Check(cancelled, "Cancellation propagates distinctly from file or state errors");
}
void Write(const fs::path &path, std::span<const uint8_t> bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
  Check(bool(file), "Fixture writes");
}
std::vector<uint8_t> Image() {
  std::vector<uint8_t> image(32768);
  std::fill_n(image.begin() + 0x7fc0, 21, ' ');
  image[0x7fd5] = 0x20;
  image[0x7fd6] = 2;
  image[0x7fd7] = 5;
  image[0x7fd8] = 3;
  image[0x7fd9] = 1;
  image[0x7fdc] = image[0x7fdd] = 0xff;
  image[0x7ffd] = 0x80;
  const uint8_t code[] = {0x78, 0xa9, 0x80, 0x8d, 0, 0x21, 0xa9, 0x5a, 0x8f, 0, 0, 0x70, 0xdb};
  std::copy(std::begin(code), std::end(code), image.begin());
  return image;
}
void Files(const fs::path &directory) {
  const auto path = directory / fs::path(u8"\u72b6\u614b.cst");
  std::vector<uint8_t> machine(128, 0x67);
  snes::core::VideoFrame picture{2, 2, {0xff123456, 0xffabcdef, 0xff987654, 0xff654321}};
  StateFile::Write(path, machine, picture);
  const auto file = StateFile::Read(path);
  Check(file.machine == machine && file.created != 0 && file.preview.width == 256 &&
            file.preview.height == 240 && file.preview.pixels.front() == 0xff123456 &&
            file.preview.pixels.back() == 0xff654321,
        "State envelope retains bytes, timestamp and a portable preview");
  Check(StateFile::ReadPreview(path).pixels == file.preview.pixels,
        "Preview-only reading retains the same validated pixels");
  const auto original = ReadFile(path, 1024 * 1024);
  for (size_t offset : {size_t(0), size_t(8), size_t(12), size_t(16), size_t(24), size_t(28),
                        size_t(32), size_t(64), original.size() - 1}) {
    auto changed = original;
    changed[offset] ^= 0x80;
    Write(path, changed);
    Reject([&] { StateFile::Read(path); }, "Changed header, machine or preview rejects");
    Reject([&] { StateFile::ReadPreview(path); }, "Preview reading validates the complete file");
  }
  Write(path, std::span(original).first(63));
  Reject([&] { StateFile::Read(path); }, "Truncated header rejects");
  Reject([&] { StateFile::ReadPreview(path); }, "Preview reading rejects truncated headers");
  Write(path, std::span(original).first(original.size() - 1));
  Reject([&] { StateFile::Read(path); }, "Truncated preview rejects");
  Reject([&] { StateFile::ReadPreview(path); }, "Preview reading rejects truncated pictures");
  auto trailer = original;
  trailer.push_back(0);
  Write(path, trailer);
  Reject([&] { StateFile::Read(path); }, "Unexpected trailer rejects");
  Reject([&] { StateFile::ReadPreview(path); }, "Preview reading rejects trailers");
  StateFile::Write(path, machine, {});
  Check(StateFile::Read(path).preview.pixels.empty(), "Power-on state has no fabricated preview");
  Check(StateFile::ReadPreview(path).pixels.empty(), "Preview reader retains absent pictures");
  Reject([&] { StateFile::Read(directory / "missing.cst"); }, "Missing file rejects");
  const auto huge = directory / "oversized.cst";
  {
    std::ofstream sparse(huge, std::ios::binary);
    sparse.seekp(StateFile::maximumMachineBytes + 1024 * 1024);
    sparse.put(0);
  }
  Reject([&] { StateFile::Read(huge); }, "Oversized file rejects before allocation");
  Reject([&] { StateFile::ReadPreview(huge); }, "Oversized preview rejects before reading payload");
}
void StreamedReads(const fs::path &directory) {
  using namespace snes::core;
  const auto path = directory / "streamed.cst";
  VideoFrame picture{3, 1, {0xff123456, 0xffabcdef, 0xff987654}};
  for (size_t size : {48u, 65535u, 65536u, 65537u, 131071u, 131073u}) {
    std::vector<uint8_t> machine(size);
    for (size_t index = 0; index < size; ++index)
      machine[index] = uint8_t(index * 17 + index / 127);
    StateFile::Write(path, machine, picture);
    const auto file = StateFile::Read(path);
    Check(file.machine == machine && StateFile::ReadPreview(path).pixels == file.preview.pixels,
          "Unaligned machine extents and preview chunk boundaries retain exact values");
  }
  const auto original = ReadFile(path, 1024 * 1024);
  for (bool preview : {false, true}) {
    for (auto phase : {StatePhase::ReadFile, StatePhase::Validate}) {
      uint64_t completed = 0;
      const StateOperation operation([&](const StateProgress &progress) {
        Check(progress.completed <= progress.total, "Stream progress stays within its file");
        if (progress.phase == StatePhase::ReadFile) {
          Check(progress.completed >= completed, "Stream progress counts increasing file bytes");
          completed = progress.completed;
        }
        return progress.phase != phase || progress.completed < 65536;
      });
      Cancelled([&] {
        if (preview)
          StateFile::ReadPreview(path, operation);
        else
          StateFile::Read(path, operation);
      });
      Check(ReadFile(path, original.size()) == original,
            "Cancelled reads leave source bytes intact");
    }
  }
  auto corrupt = original;
  corrupt[64 + 65537] ^= 1;
  Write(path, corrupt);
  Reject([&] { StateFile::ReadPreview(path); }, "Preview integrity covers skipped machine bytes");
}
void Catalog(const fs::path &directory) {
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(directory / "same.sfc");
  StateStorage storage(request, directory / "states");
  const auto path = storage.Path({false, "Before boss"});
  StateFile::Write(path, std::vector<uint8_t>(48), {});
  StateFile::Write(storage.Path(StateStorage::Quick(9)), std::vector<uint8_t>(48, 1), {});
  StateFile::Write(storage.Path(StateStorage::Quick(1)), std::vector<uint8_t>(48, 2), {});
  Check(storage.List(false).size() == 1 && storage.List(true).size() == 2 &&
            storage.List(true)[0].key == StateStorage::Quick(1),
        "Managed and quick categories list separately with numeric slot order");
  storage.Rename({false, "Before boss"}, "After boss");
  const auto renamed = storage.Path({false, "After boss"});
  Check(!fs::exists(path) && fs::exists(renamed), "Rename retains the state file");
  StateFile::Write(path, std::vector<uint8_t>(48, 7), {});
  const auto previous = ReadFile(renamed, 1024);
  Reject([&] { storage.Rename({false, "Before boss"}, "After boss"); },
         "Rename never overwrites another state");
  Check(ReadFile(renamed, 1024) == previous && fs::exists(path),
        "Rejected rename retains both states");
  Reject([&] { storage.Rename(StateStorage::Quick(1), "Moved"); }, "Quick slot names are fixed");
  for (const char *name : {"", "..", "../elsewhere", "a/b", "a\\b", "a:stream", "trailing.",
                           " leading", "trailing ", "CON", "aux.txt", "lpt9", "bad\nname"})
    Reject([&] { storage.Path({false, name}); }, "Portable state names cannot escape their folder");
  Reject([&] { storage.Path({true, "11"}); }, "Unknown quick slot rejects");
  storage.Remove({false, "Before boss"});
  Check(!fs::exists(path) && fs::exists(renamed), "Remove targets only the selected state");
  std::set<fs::path> identities;
  identities.insert(storage.Directory());
  for (const auto kind : {LaunchRequest::Kind::Handheld, LaunchRequest::Kind::SufamiTurbo,
                          LaunchRequest::Kind::Broadcast, LaunchRequest::Kind::Campus92,
                          LaunchRequest::Kind::PowerFest94}) {
    auto variant = request;
    variant.kind = kind;
    for (unsigned slot = 1; slot < 4; ++slot)
      variant.paths[slot] = snes::core::PathText(directory / ("slot" + std::to_string(slot)));
    Check(identities.insert(StateStorage(variant, directory / "states").Directory()).second,
          "Launch modes have distinct state namespaces");
    for (unsigned slot = 0; slot < 4; ++slot) {
      auto other = variant;
      other.paths[slot] += "-other";
      Check(identities.insert(StateStorage(other, directory / "states").Directory()).second,
            "Each inserted asset participates in state identity");
    }
    variant.competitionDip = 3;
    Check(identities.insert(StateStorage(variant, directory / "states").Directory()).second,
          "Competition settings participate in state identity");
  }
}
void CancelFiles(const fs::path &directory) {
  using namespace snes::core;
  const auto path = directory / "atomic-cancel.bin";
  const std::vector<uint8_t> original(1024 * 1024 + 19, 0x37), next(original.size(), 0xa1);
  WriteFileAtomically(path, original);
  const auto entries = [&] {
    std::set<fs::path> paths;
    for (const auto &entry : fs::directory_iterator(directory))
      paths.insert(entry.path());
    return paths;
  };
  const auto before = entries();
  for (bool final : {false, true}) {
    const StateOperation operation([&](const StateProgress &progress) {
      Check(progress.phase == StatePhase::WriteFile && progress.completed <= progress.total,
            "File writes report their current byte extent");
      return final ? progress.completed != progress.total : progress.completed < 65536;
    });
    Cancelled([&] { WriteFileAtomically(path, next, operation); });
    Check(ReadFile(path, original.size()) == original && entries() == before,
          "Cancelled writes retain the old bytes and remove their own temporary file");
  }
  const auto missing = directory / "cancelled-new.bin";
  Cancelled([&] {
    WriteFileAtomically(missing, next, StateOperation([](const StateProgress &progress) {
                          return progress.completed != progress.total;
                        }));
  });
  Check(!fs::exists(missing) && entries() == before,
        "Cancellation before publication leaves no new file or temporary artifact");
  Cancelled([&] {
    ReadFile(path, original.size(), StateOperation([](const StateProgress &progress) {
               return progress.completed < 65536;
             }));
  });
  Check(ReadFile(path, original.size()) == original, "Cancelled reads retain the source file");
  WriteFileAtomically(path, next);
  Check(ReadFile(path, next.size()) == next, "An atomic write can be retried after cancellation");
}
void Workflow(const fs::path &directory, const fs::path &exportFile, const fs::path &importFile) {
  const auto rom = directory / "game.sfc";
  Write(rom, Image());
  Session session(snes::tests::DeterministicConsoleConfig(), {},
                  {snes::core::ControllerDevice::Gamepad, snes::core::ControllerDevice::Gamepad});
  LaunchRequest request;
  request.paths[0] = snes::core::PathText(rom);
  session.Open(request);
  auto *machine = session.Machine();
  const auto startup = machine->CaptureState();
  const auto quick = session.States()->Path(StateStorage::Quick(0));
  session.SaveState(quick);
  Check(machine->CaptureState() == startup && StateFile::Read(quick).preview.pixels.empty(),
        "Saving at power-on consumes no input, clocks or fabricated frames");
  machine->StepFrame();
  machine->GetAutoJoypad().Ports().Configure(0, snes::core::ControllerDevice::Mouse);
  machine->GetAutoJoypad().Ports().SetLatch(true);
  machine->GetAutoJoypad().Ports().SetLatch(false);
  machine->GetAutoJoypad().Ports().Read(0);
  session.SaveState(quick);
  const auto first = StateFile::Read(quick).machine;
  if (!exportFile.empty())
    StateFile::Write(exportFile, first, machine->LastFrame());
  if (!importFile.empty()) {
    session.LoadState(importFile);
    Check(machine->CaptureState() == first,
          "A state file from another build restores the same complete continuation");
    Check(StateFile::Read(importFile).preview.pixels == StateFile::Read(quick).preview.pixels,
          "Transferred preview pixels retain their exact byte layout");
  }
  Check(!StateFile::Read(quick).preview.pixels.empty(), "Completed picture is stored for preview");
  auto devices = session.Devices();
  devices.ports[0] = snes::core::ControllerDevice::Multitap;
  session.SetDevices(devices);
  machine->StepFrame();
  machine->GetBus().Write(0x7e8123, 0xa7);
  machine->GetBus().Write(0x700000, 0xb9);
  const auto second = machine->CaptureState();
  session.Flush();
  const auto battery = ReadFile(directory / "game.srm", 8192);
  session.LoadState(quick);
  Check(machine == session.Machine() && machine->CaptureState() == first &&
            session.Devices().ports[0] == snes::core::ControllerDevice::Mouse &&
            session.CanUndoState() && !session.CanRedoState(),
        "Load restores all state and port packet wiring without replacing the machine");
  Check(ReadFile(directory / "game.srm", 8192) == battery,
        "State load does not load or flush battery files");
  session.UndoState();
  Check(machine->CaptureState() == second && !session.CanUndoState() && session.CanRedoState() &&
            session.Devices().ports[0] == snes::core::ControllerDevice::Multitap,
        "Undo restores the pre-load execution and live device selection");
  const auto previousFile = ReadFile(quick, 8 * 1024 * 1024);
  using namespace snes::core;
  for (auto phase :
       {StatePhase::ReadFile, StatePhase::Validate, StatePhase::Capture, StatePhase::Commit}) {
    Cancelled([&] {
      session.LoadState(quick, StateOperation([phase](const StateProgress &progress) {
                          return progress.phase != phase;
                        }));
    });
    Check(machine->CaptureState() == second && session.CanRedoState() && !session.CanUndoState() &&
              session.Devices().ports[0] == ControllerDevice::Multitap,
          "Cancelled load retains live execution, device selection and previous redo");
  }
  for (auto phase : {StatePhase::Capture, StatePhase::WriteFile}) {
    Cancelled([&] {
      session.SaveState(quick, StateOperation([phase](const StateProgress &progress) {
                          return progress.phase != phase || (phase == StatePhase::WriteFile &&
                                                             progress.completed != progress.total);
                        }));
    });
    Check(ReadFile(quick, 8 * 1024 * 1024) == previousFile && machine->CaptureState() == second &&
              session.CanRedoState() && !session.CanUndoState(),
          "Cancelled save retains the old slot and previous redo history");
  }
  Cancelled([&] {
    session.RedoState(StateOperation(
        [](const StateProgress &progress) { return progress.phase != StatePhase::Commit; }));
  });
  Check(machine->CaptureState() == second && session.CanRedoState(),
        "Cancelled redo retains its state and can be retried");
  const auto corrupt = directory / "corrupt.cst";
  auto invalid = first;
  invalid[8] ^= 1;
  StateFile::Write(corrupt, invalid, {});
  Reject([&] { session.LoadState(corrupt); }, "Unsupported machine version rejects");
  Check(machine->CaptureState() == second && session.CanRedoState() && !session.CanUndoState(),
        "Invalid machine state rejects before advancing execution or discarding redo");
  Write(corrupt, std::vector<uint8_t>(64));
  Reject([&] { session.LoadState(corrupt); }, "Malformed state envelope rejects");
  Check(machine->CaptureState() == second && session.CanRedoState(),
        "Malformed file retains live execution and redo");
  session.RedoState();
  Check(machine->CaptureState() == first && session.CanUndoState() && !session.CanRedoState(),
        "Redo restores the loaded state and permits another undo");
  const auto oldDirectory = session.States()->Directory();
  Reject(
      [&] {
        session.SetStateDirectory(directory / "new-states",
                                  [] { throw std::runtime_error("disk full"); });
      },
      "A failed preferences write rejects state-directory changes");
  Check(session.States()->Directory() == oldDirectory && machine->CaptureState() == first,
        "Rejected state path retains the active destinations and machine");
  session.SetStateDirectory(directory / "new-states");
  Check(session.States()->Directory() != oldDirectory && session.CanUndoState(),
        "Changing state directory preserves undo and live execution");
  const auto blocked = directory / "blocked.cst";
  fs::create_directory(blocked);
  Reject([&] { session.SaveState(blocked); }, "Failed atomic replacement reports a write error");
  Check(fs::is_directory(blocked) && session.CanUndoState(),
        "Failed save preserves undo and target");
  session.Open(request);
  Check(!session.CanUndoState() && !session.CanRedoState(),
        "New launch clears the previous undo history");
  session.Close();
  Check(!session.States(), "Unloaded session has no active catalog");
}
} // namespace

int main(int argc, char **argv) {
  const auto directory =
      fs::temp_directory_path() /
      ("cupid-state-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    Files(directory);
    StreamedReads(directory);
    Catalog(directory);
    CancelFiles(directory);
    Workflow(directory, argc > 1 ? snes::core::FilePath(argv[1]) : fs::path{},
             argc > 2 ? snes::core::FilePath(argv[2]) : fs::path{});
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
  return 0;
}
