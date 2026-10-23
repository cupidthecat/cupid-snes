#include "audio/MsuStateFixture.hpp"
#include "snes/core/FilePath.hpp"

#include <cstdio>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace snes::tests::msu_state;
using snes::core::Msu1;
using snes::core::detail::StateArchive;
namespace {
const char *stage = "startup";
unsigned position = 0;
#ifdef _WIN32
std::array<int64_t, 2> FileTimes(const std::filesystem::path &path) {
  const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return {};
  FILE_BASIC_INFO info{};
  const bool valid = GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info));
  CloseHandle(handle);
  return valid ? std::array<int64_t, 2>{info.LastWriteTime.QuadPart, info.ChangeTime.QuadPart}
               : std::array<int64_t, 2>{};
}
#endif
template <class T> std::vector<uint8_t> Capture(T &device) {
  StateArchive archive;
  device.SerializeState(archive);
  return archive.Bytes();
}
template <class T> void Restore(T &device, std::span<const uint8_t> bytes) {
  StateArchive archive(bytes);
  device.SerializeState(archive);
  archive.Commit();
}
template <class T> void Reject(T &device, std::span<const uint8_t> bytes) {
  const auto before = Capture(device);
  bool failed = false;
  try {
    Restore(device, bytes);
  } catch (const std::runtime_error &) {
    failed = true;
  }
  Check(failed, "Invalid MSU state must reject");
  Check(Capture(device) == before, "Rejected MSU state preserves live execution and buffers");
}
void Truncation(Msu1 &chip) {
  const auto saved = Capture(chip);
  for (size_t n = 0; n < saved.size(); n += std::max(size_t{1}, saved.size() / 17))
    Reject(chip, std::span(saved).first(n));
  Reject(chip, std::span(saved).first(saved.size() - 1));
  auto extra = saved;
  extra.push_back(0);
  Reject(chip, extra);
}
uint64_t Continuation() {
  stage = "continuation";
  uint64_t count = 0;
  for (unsigned profile = 0; profile < 16; ++profile) {
    Media a("source.sfc", profile & 1 ? 20013 : 0),
        b(std::filesystem::path(u8"target.\u8056.sfc"), profile & 1 ? 20013 : 0);
    Fixture first(a, profile), second(b, profile);
    for (unsigned cut = 0; cut < 768; ++cut) {
      position = profile * 10000 + cut;
      first.Step(cut);
      const auto saved = Capture(first);
      Check(Capture(first) == saved, "Repeated MSU capture retains sample and port positions");
      second.Step(cut + 19);
      Restore(second, saved);
      for (unsigned future : {cut + 1, cut + 7, cut + 97}) {
        first.Step(future);
        second.Step(future);
        Check(Capture(first) == Capture(second),
              "MSU data latches, caches, PCM and resume phases continue exactly");
      }
      Restore(first, saved);
      ++count;
    }
    Truncation(first.chip);
  }
  return count;
}
void ActiveTrackAndBinding() {
  stage = "open track";
  Media source, target(std::filesystem::path(u8"relocated.\u8056.sfc"));
  Msu1 first, second;
  Check(first.Open(source.rom) && second.Open(target.rom), "Open distinct matching packs");
  Select(first, 1);
  (void)first.Clock();
  (void)first.Clock();
  first.Write(0x2004, 2);
  const auto saved = Capture(first);
  const auto expected = first.Clock();
  first.Close();
  auto different = Media::Pcm(91, 5013);
  source.Write(source.Track(1), different);
  Select(second, 2);
  Restore(second, saved);
  Check(second.Clock() == expected,
        "Low-only track writes retain the old file and use target media paths");
  const auto closed = Capture(first);
  Restore(second, closed);
  Check(!second.IsOpen(), "A closed stream checkpoint releases target resources");
  Restore(second, saved);
  Check(second.IsOpen() && second.Clock() == expected,
        "Target media bindings survive closed-state restoration");
  Msu1 missing;
  Reject(missing, saved);
}
void CacheBoundaries() {
  stage = "cache boundary";
  Media a, b;
  Msu1 first, second;
  Check(first.Open(a.rom) && second.Open(b.rom), "Open cache boundary packs");
  for (uint32_t offset : {0u, 1u, 8191u, 8192u, 16383u, 20012u, 20013u, 0x80000003u, 0xffffffffu}) {
    position = offset;
    Seek(first, offset);
    Select(first, 1);
    for (unsigned n = 0; n < 2041; ++n)
      (void)first.Clock();
    const auto saved = Capture(first);
    Restore(second, saved);
    for (unsigned n = 0; n < 1000; ++n) {
      Check(first.Read(0x2001) == second.Read(0x2001),
            "Restored data cache crosses refill and EOF boundaries");
      Check(first.Clock(n & 1) == second.Clock(n & 1),
            "Restored PCM cache crosses refill boundaries");
    }
    Check(Capture(first) == Capture(second),
          "Cache-boundary continuation retains exact private state");
  }
}
void MediaFailures() {
  stage = "media mismatch";
  Media a, b;
  Msu1 first, second;
  Check(first.Open(a.rom) && second.Open(b.rom), "Open media validation packs");
  Select(first, 1);
  Select(second, 2);
  const auto saved = Capture(first);
  auto changed = Media::Pcm(71, 5013);
  const auto timestamp = std::filesystem::last_write_time(b.Track(1));
  b.Write(b.Track(1), changed);
  std::filesystem::last_write_time(b.Track(1), timestamp);
  Reject(second, saved);
  // A resource failure must poison the archive even when the caller catches it.
  StateArchive archive(saved);
  bool failed = false;
  try {
    second.SerializeState(archive);
  } catch (const std::runtime_error &) {
    failed = true;
  }
  Check(failed, "Changed PCM media rejects during validation");
  failed = false;
  try {
    archive.Expect(uint8_t{0});
  } catch (const std::runtime_error &) {
    failed = true;
  }
  Check(failed, "A caught media preparation error invalidates the entire archive");
  b.Write(b.Track(1), Media::Pcm(1, 5013));
  Restore(second, saved);
  auto corrupt = saved;
  // The section tag and exact length protect the schema before any live field changes.
  corrupt[0] ^= 1;
  Reject(second, corrupt);
  corrupt = saved;
  corrupt[4] ^= 1;
  Reject(second, corrupt);
}
void CachedMediaChanges() {
  stage = "cached media mutation";
  for (bool pcm : {false, true}) {
    position = pcm;
    Media media;
    Msu1 chip;
    Check(chip.Open(media.rom), "Open mutable media pack");
    Select(chip, 1);
    (void)chip.Read(0x2001);
    (void)chip.Clock();
    const auto saved = Capture(chip);
    Check(Capture(chip) == saved, "Warm the media identity cache");
    const auto path = pcm ? media.Track(1) : media.Data();
    const auto timestamp = std::filesystem::last_write_time(path);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(pcm ? 10000 : 19000);
    file.put(char(0x71));
    file.close();
    Check(bool(file), "Modify media beyond the emulated read cache");
    std::filesystem::last_write_time(path, timestamp);
    const auto changed = Capture(chip);
    const size_t key = pcm ? 8331 : 78;
    if (std::equal(saved.begin() + key, saved.begin() + key + 32, changed.begin() + key)) {
      if (!pcm) {
        Seek(chip, 19000);
        std::fprintf(stderr, "Held data byte after mutation: %u (expected 113)\n",
                     unsigned(chip.Read(0x2001)));
      }
      Msu1 fresh;
      Check(fresh.Open(media.rom), "Open a fresh media identity witness");
      Select(fresh, 1);
      const auto reopened = Capture(fresh);
      std::fprintf(stderr, "Unchanged media identity after mutation; fresh handle differs: %u\n",
                   unsigned(!std::equal(saved.begin() + key, saved.begin() + key + 32,
                                        reopened.begin() + key)));
#ifdef _WIN32
      const auto timesAfter = FileTimes(path);
      std::fprintf(stderr, "File times: saved write-count %lld, current write %lld, change %lld\n",
                   (long long)timestamp.time_since_epoch().count(), (long long)timesAfter[0],
                   (long long)timesAfter[1]);
#endif
    }
    Check(changed != saved,
          "Changed bytes invalidate a cached digest even with the same size and modification time");
    Reject(chip, saved);
  }
}
void BusyMedia() {
#ifdef _WIN32
  stage = "busy media";
  for (bool pcm : {false, true}) {
    position = pcm;
    Media a, b;
    Msu1 first, second;
    Check(first.Open(a.rom) && second.Open(b.rom), "Open media lease witnesses");
    Select(first, 1);
    Select(second, 2);
    const auto saved = Capture(first), before = Capture(second);
    std::fstream writer(pcm ? b.Track(1) : b.Data(),
                        std::ios::binary | std::ios::in | std::ios::out);
    Check(bool(writer), "Open a competing host writer");
    StateArchive archive(saved);
    bool failed = false;
    try {
      second.SerializeState(archive);
    } catch (const std::runtime_error &) {
      failed = true;
    }
    Check(failed, "An active host writer prevents checkpoint media preparation");
    failed = false;
    try {
      archive.Commit();
    } catch (const std::runtime_error &) {
      failed = true;
    }
    Check(failed, "A caught media lease failure cannot commit a partial restore");
    writer.close();
    Check(Capture(second) == before, "A failed media lease retains target handles and state");
    Restore(second, saved);
    Check(first.Clock() == second.Clock(), "Restore succeeds after the competing writer closes");
  }
#endif
}
void ChangedFileExtents() {
  stage = "changed file extent";
  for (bool pcm : {false, true})
    for (bool grow : {false, true}) {
      Media a, b;
      Msu1 first, second;
      Check(first.Open(a.rom), "Open source before a host media resize");
      Select(first, 1);
      (void)first.Read(0x2001);
      (void)first.Clock();
      auto data = pcm ? Media::Pcm(71, grow ? 6013 : 2300)
                      : std::vector<uint8_t>(grow ? 24013 : 9200, 0x73);
      a.Write(pcm ? a.Track(1) : a.Data(), data);
      b.Write(pcm ? b.Track(1) : b.Data(), data);
      Check(second.Open(b.rom), "Open target after the same host media resize");
      Select(second, 1);
      const auto saved = Capture(first);
      Restore(second, saved);
      for (unsigned n = 0; n < 25000; ++n) {
        Check(first.Read(0x2001) == second.Read(0x2001),
              "Restored data retains old cache bytes and the original emulated EOF limit");
        Check(first.Clock() == second.Clock(),
              "Restored PCM retains old cache bytes and the original emulated EOF limit");
      }
      Check(Capture(first) == Capture(second),
            "Host file length and emulated length stay distinct");
    }
}
void RenamedOpenMedia() {
#ifndef _WIN32
  stage = "renamed open media";
  for (bool pcm : {false, true}) {
    position = pcm;
    Media a, b;
    Msu1 first, second;
    Check(first.Open(a.rom) && second.Open(b.rom), "Open rename witnesses");
    Select(first, 1);
    (void)first.Clock();
    (void)first.Read(0x2001);
    const auto saved = Capture(first);
    const auto original = pcm ? a.Track(1) : a.Data();
    std::fstream writer(original, std::ios::binary | std::ios::in | std::ios::out);
    Check(bool(writer), "Open a writer bound to the same held resource");
    auto held = original;
    held += ".held";
    a.Write(held, {});
    std::filesystem::remove(held);
    std::filesystem::rename(original, held);
    auto changed = pcm ? Media::Pcm(71, 5013) : std::vector<uint8_t>(20013, 0x73);
    a.Write(original, changed);
    Check(Capture(first) == saved, "A replaced pathname does not change the held file's identity");
    Restore(second, saved);
    Check(first.Clock() == second.Clock() && first.Read(0x2001) == second.Read(0x2001),
          "Renamed held files continue through a matching target binding");
    const auto before = Capture(first);
    const auto timestamp = std::filesystem::last_write_time(held);
    writer.seekp(10000);
    writer.put(char(0x71));
    writer.flush();
    Check(bool(writer), "Modify the held resource through its original descriptor");
    std::filesystem::last_write_time(held, timestamp);
    Reject(first, before);
    writer.close();
    Check(bool(writer), "Close the held-resource writer");
  }
#endif
}
void PlaybackEdges() {
  stage = "playback edge";
  Media a, b;
  Msu1 first, second;
  Check(first.Open(a.rom) && second.Open(b.rom), "Open playback edge packs");
  for (uint16_t track : {0, 1, 2, 3, 4, 5, 6, 7, 65535})
    for (uint8_t control : {0, 1, 2, 3, 4, 5, 6, 7}) {
      position = unsigned(track) * 8 + control;
      Select(first, track, control);
      for (unsigned cut = 0; cut < 5024; ++cut) {
        if (cut < 4 || cut >= 5009 || cut == 2045 || cut == 4093) {
          const auto saved = Capture(first);
          Restore(second, saved);
          for (unsigned n = 0; n < 7; ++n) {
            Check(first.Clock(n & 1) == second.Clock(n & 1),
                  "PCM EOF, loop seek silence and partial samples retain the next input tick");
            Check(first.Read(0x2000) == second.Read(0x2000),
                  "Playback and error flags retain the same transition");
          }
          Check(Capture(first) == Capture(second), "Playback edges retain exact device state");
          Restore(first, saved);
        }
        (void)first.Clock();
      }
      first.Write(0x2007, 4);
      const auto resume = Capture(first);
      Select(second, 2);
      Restore(second, resume);
      for (uint16_t next : {uint16_t{2}, track, track}) {
        Select(first, next);
        Select(second, next);
        for (unsigned n = 0; n < 3; ++n)
          Check(first.Clock() == second.Clock(),
                "Resume survives unrelated and matching selections");
      }
      Check(Capture(first) == Capture(second), "Resume slot is consumed by the same selection");
    }
}
void InvalidFieldsAndMoves() {
  stage = "invalid state and moves";
  Media a, b;
  Msu1 first, second;
  Check(first.Open(a.rom) && second.Open(b.rom), "Open invalid-state packs");
  Select(first, 1);
  Select(second, 2);
  (void)first.Clock();
  const auto saved = Capture(first);
  Check(saved.size() == 16555, "MSU section uses fixed portable field widths");
  for (size_t offset : {size_t(24), size_t(42), size_t(49), size_t(50 + 7), size_t(58 + 7),
                        size_t(66 + 7), size_t(74 + 1), size_t(78), size_t(8302), size_t(8303 + 7),
                        size_t(8311 + 7), size_t(8319 + 7), size_t(8327 + 1), size_t(8331)}) {
    auto corrupt = saved;
    corrupt[offset] = 0xff;
    Reject(second, corrupt);
  }
  for (size_t offset : {size_t(34), size_t(38), size_t(45)}) {
    auto corrupt = saved;
    corrupt[offset] |= 1;
    Reject(second, corrupt);
  }
  auto late = saved;
  late[24] = 8;
  Reject(second, late);
  // Reject after resource preparation; the original track must still be audible.
  Msu1 control;
  Check(control.Open(b.rom), "Open independent rejection witness");
  Select(control, 2);
  Check(second.Clock() == control.Clock(), "Late rejection retains the original live file handle");
  Msu1 moved(std::move(first));
  Check(Capture(moved) == saved, "Move construction retains streams, binding and cache ownership");
  second = std::move(moved);
  Check(Capture(second) == saved, "Move assignment retains complete playback state");
  Check(first.Open(a.rom), "A moved-from device can open another pack");
}
void Portable(const char *path) {
  std::ofstream output(snes::core::FilePath(path), std::ios::binary);
  uint64_t size = 0;
  for (unsigned profile = 0; profile < 16; ++profile) {
    Media media("portable.sfc", profile & 1 ? 20013 : 0);
    Fixture fixture(media, profile);
    for (unsigned n = 0; n < 37; ++n)
      fixture.Step(n);
    const auto bytes = Capture(fixture);
    output.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
    size += bytes.size();
  }
  Check(bool(output), "Write portable MSU fixture");
  std::printf("Portable MSU fixture: %llu bytes\n", (unsigned long long)size);
}
} // namespace
int main(int argc, char **argv) {
  try {
    ActiveTrackAndBinding();
    CacheBoundaries();
    MediaFailures();
    BusyMedia();
    CachedMediaChanges();
    ChangedFileExtents();
    RenamedOpenMedia();
    PlaybackEdges();
    InvalidFieldsAndMoves();
    const auto cuts = Continuation();
    if (argc == 2)
      Portable(argv[1]);
    std::printf("MSU portable state: %llu continuation checks passed\n", (unsigned long long)cuts);
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s/%u: %s\n", stage, position, error.what());
    return 1;
  }
}
