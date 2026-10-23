#include "snes/core/Msu1.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

using snes::core::Msu1;
using Sample = Msu1::StereoSample;

namespace {

void Check(bool condition, const char* description) {
    if (!condition)
        throw std::runtime_error(description);
}

// All fixtures live under the test's working directory. Only paths created
// by this fixture are removed, and streams in each test die before it does.
class Fixture {
  public:
    explicit Fixture(const std::filesystem::path& name = "game.sfc") {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            auto candidate =
                std::filesystem::current_path() /
                ("msu1-fixture-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                directory_ = std::move(candidate);
                break;
            }
            if (error)
                throw std::filesystem::filesystem_error("Create MSU-1 fixture", candidate, error);
        }
        Check(!directory_.empty(), "A unique fixture directory can be created");
        rom = directory_ / name;
    }

    ~Fixture() {
        std::error_code error;
        for (const auto& path : files_)
            std::filesystem::remove(path, error);
        std::filesystem::remove(directory_, error);
    }

    std::filesystem::path DataPath() const {
        auto result = rom;
        result.replace_extension(".msu");
        return result;
    }

    std::filesystem::path TrackPath(uint16_t track) const {
        auto result = rom;
        result.replace_extension();
        result += "-" + std::to_string(track) + ".pcm";
        return result;
    }

    void Data(std::span<const uint8_t> bytes = {}) {
        Write(DataPath(), bytes);
    }

    void RawTrack(uint16_t track, std::span<const uint8_t> bytes) {
        Write(TrackPath(track), bytes);
    }

    void Pcm(uint16_t track, std::span<const Sample> frames, uint32_t loop = 0) {
        std::vector<uint8_t> bytes(8 + frames.size() * 4);
        bytes[0] = 'M';
        bytes[1] = 'S';
        bytes[2] = 'U';
        bytes[3] = '1';
        for (unsigned byte = 0; byte < 4; ++byte)
            bytes[4 + byte] = static_cast<uint8_t>(loop >> (byte * 8));
        for (size_t frame = 0; frame < frames.size(); ++frame) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                const auto bits = static_cast<uint16_t>(frames[frame][channel]);
                bytes[8 + frame * 4 + channel * 2] = static_cast<uint8_t>(bits);
                bytes[9 + frame * 4 + channel * 2] = static_cast<uint8_t>(bits >> 8);
            }
        }
        RawTrack(track, bytes);
    }

    void SparseData() {
        static_assert(sizeof(std::streamoff) >= 8);
        const std::array<uint8_t, 1> first{0x19};
        Data(first);
#ifdef _WIN32
        // Mark the file sparse before extending it: the test must never
        // allocate four gigabytes of physical disk space on Windows.
        const auto path = DataPath();
        const HANDLE handle =
            CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(handle != INVALID_HANDLE_VALUE, "Open sparse fixture handle");
        DWORD returned = 0;
        const bool sparse = DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                                            &returned, nullptr) != 0;
        CloseHandle(handle);
        Check(sparse, "Mark large fixture sparse before extending it");
#endif
        std::fstream file(DataPath(), std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(0x80000003ull));
        file.put(static_cast<char>(0x5a));
        file.seekp(static_cast<std::streamoff>(0xffffffffull));
        file.put(static_cast<char>(0xa5));
        file.close();
        Check(!file.fail(), "Write sparse data sentinels");
        Check(std::filesystem::file_size(DataPath()) == 0x100000000ull,
              "Sparse fixture reaches the end of the 32-bit seek space");
    }

    std::filesystem::path rom;

  private:
    void Write(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
        if (std::find(files_.begin(), files_.end(), path) == files_.end())
            files_.push_back(path);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        Check(file.is_open(), "Open generated fixture");
        if (!bytes.empty())
            file.write(reinterpret_cast<const char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        file.close();
        Check(!file.fail(), "Write generated fixture");
    }

    std::filesystem::path directory_;
    std::vector<std::filesystem::path> files_;
};

void Track(Msu1& chip, uint16_t track) {
    chip.Write(0x2004, static_cast<uint8_t>(track));
    chip.Write(0x2005, static_cast<uint8_t>(track >> 8));
}

void Seek(Msu1& chip, uint32_t offset) {
    for (unsigned byte = 0; byte < 4; ++byte)
        chip.Write(0x2000 + byte, static_cast<uint8_t>(offset >> (byte * 8)));
}

void Play(Msu1& chip, uint16_t track, uint8_t control = 1, uint8_t volume = 255) {
    Track(chip, track);
    chip.Write(0x2006, volume);
    chip.Write(0x2007, control);
}

Sample Tick(Msu1& chip, bool muted = false) {
    const auto frame = chip.Clock(muted);
    return {int16_t(std::lround(frame[0] * 32768)), int16_t(std::lround(frame[1] * 32768))};
}

void DiscoveryMappingAndLifecycle() {
    Fixture fixture;
    Msu1 chip;
    Check(!chip.IsOpen() && chip.Read(0x2002, 0x73) == 0x73,
          "Absent device leaves open bus unchanged");
    chip.Write(0x2007, 3);
    Check(Tick(chip) == Sample{}, "Absent device produces silence");
    std::string error;
    Check(!chip.Open(fixture.rom, &error) && !error.empty(),
          "Missing data file reports discovery failure");
    fixture.Data();
    const std::array frames{Sample{1200, -2400}, Sample{3600, -4800}};
    fixture.Pcm(0, frames);
    Check(chip.Open(fixture.rom, &error) && error.empty(),
          "Empty data file enables an audio-only pack");
    Check(chip.Read(0x2000) == 0x02 && chip.Read(0x2001) == 0,
          "Revision two is ready immediately and empty data reads zero");
    const std::string signature = "S-MSU1";
    for (uint32_t bank = 0; bank < 256; ++bank) {
        const bool mapped = (bank & 0x40) == 0;
        for (uint32_t port = 0; port < 8; ++port) {
            const auto address = (bank << 16) | 0x2000 | port;
            Check(Msu1::Selects(address) == mapped,
                  "All and only the documented banks select MSU-1");
            if (port >= 2)
                Check(chip.Read(address, 0x6c) == (mapped ? signature[port - 2] : 0x6c),
                      "Identification signature is mirrored through both I/O bank ranges");
        }
        Check(!Msu1::Selects((bank << 16) | 0x1fff) && !Msu1::Selects((bank << 16) | 0x2008),
              "Register window has exact boundaries");
    }
    Check(!Msu1::Selects(0x1002002) && chip.Read(0x1002002, 0x6c) == 0x6c,
          "Addresses beyond the CPU address space do not alias the device");
    chip.Write(0x402004, 99);
    chip.Write(0x402005, 99);
    Check(chip.Read(0x2000) == 2, "Unmapped writes do not select tracks");

    Play(chip, 0);
    Check(Tick(chip) == frames[0], "Track zero starts at PCM data instead of its header");
    chip.Write(0x2007, 4);
    chip.Reset();
    Check(chip.IsOpen() && chip.Read(0x2000) == 2,
          "Reset retains attachment and clears all status flags");
    Track(chip, 0);
    chip.Write(0x2007, 1);
    Check(Tick(chip) == Sample{}, "Reset clears volume and continues silent playback");
    Play(chip, 0);
    Check(Tick(chip) == frames[0], "Reset invalidates the saved resume point");
    chip.Close();
    Check(!chip.IsOpen() && chip.Read(0x2002, 0x91) == 0x91 && Tick(chip) == Sample{},
          "Close removes both data and audio state");
    Check(chip.Open(fixture.rom), "Pack can reopen after close");
    Play(chip, 0);
    Check(!chip.Open(fixture.rom.parent_path() / "missing.sfc") && !chip.IsOpen(),
          "Failed replacement closes the preceding pack");
    Check(!chip.Open({}) && !chip.IsOpen(), "An empty ROM path cannot discover an unrelated pack");
}

void DataLatchesAndCacheBoundaries() {
    Fixture fixture;
    std::vector<uint8_t> bytes(20013);
    for (size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(index * 29 + 7);
    fixture.Data(bytes);
    Msu1 chip;
    Check(chip.Open(fixture.rom), "Open generated data stream");
    Check(chip.Read(0x2001) == bytes[0], "Data starts at offset zero");
    chip.Write(0x2000, 0x34);
    chip.Write(0x2001, 0x12);
    chip.Write(0x2002, 0);
    Check(chip.Read(0x2001) == bytes[1], "Low seek bytes only update the latch");
    chip.Write(0x802003, 0);
    Check(chip.Read(0x3f2001) == bytes[0x1234], "High seek byte commits the shared 32-bit address");
    Seek(chip, 0);
    for (const auto byte : bytes)
        Check(chip.Read(0x2001) == byte, "Sequential reads cross multiple fixed cache boundaries");
    Check(chip.Read(0x2001) == 0 && chip.Read(0x2001) == 0, "Repeated reads at EOF return zero");
    Seek(chip, 0xffffffff);
    Check(chip.Read(0x2001) == 0 && chip.Read(0x2000) == 0x0a,
          "Seek past EOF completes without busy state");
    Seek(chip, 8191);
    for (size_t index = 8191; index < 8205; ++index)
        Check(chip.Read(0x2001) == bytes[index],
              "Seeking recovers after EOF and across cache boundaries");
    chip.Reset();
    Check(chip.Read(0x2001) == bytes[0], "Reset rewinds the data position");
}

void SparseFourGigabyteData() {
    Fixture fixture;
    fixture.SparseData();
    Msu1 chip;
    Check(chip.Open(fixture.rom), "Open a sparse four-gigabyte data file");
    Seek(chip, 0x80000003);
    Check(chip.Read(0x2001) == 0x5a, "Data seek above two gigabytes is unsigned and 64-bit safe");
    Check(chip.Read(0x2001) == 0, "Sparse holes read as data without allocating their span");
    Seek(chip, 0xffffffff);
    Check(chip.Read(0x2001) == 0xa5,
          "All four seek bytes address the final byte of four gigabytes");
    Check(chip.Read(0x2001) == 0 && chip.Read(0x2001) == 0,
          "Sequential EOF after the maximum seek does not wrap to byte zero");
    Seek(chip, 0);
    Check(chip.Read(0x2001) == 0x19, "A small seek works after the largest possible seek");
}

void TrackValidationAndSelection() {
    Fixture fixture(std::filesystem::path(u8"game.part \u03c0.sfc"));
    fixture.Data();
    const std::array first{Sample{111, -222}, Sample{333, -444}};
    const std::array second{Sample{-555, 666}, Sample{-777, 888}};
    fixture.Pcm(1, first);
    fixture.Pcm(260, second);
    fixture.RawTrack(2, std::array<uint8_t, 12>{'B', 'A', 'D', '1'});
    fixture.RawTrack(3, std::array<uint8_t, 4>{'M', 'S', 'U', '1'});
    fixture.RawTrack(4, std::array<uint8_t, 7>{'M', 'S', 'U', '1'});
    fixture.Pcm(5, {});
    fixture.RawTrack(6, std::array<uint8_t, 9>{'M', 'S', 'U', '1', 0, 0, 0, 0, 0x92});
    fixture.RawTrack(7, std::array<uint8_t, 11>{'M', 'S', 'U', '1', 0, 0, 0, 0, 0x34, 0x92, 0x56});
    Msu1 chip;
    Check(chip.Open(fixture.rom), "Dotted Unicode file stem discovers its sibling data file");
    Check(chip.Read(0x2000) == 0x0a, "Power attempts track zero and reports its missing file");
    for (uint16_t track : {2, 3, 4, 999}) {
        Track(chip, track);
        Check(chip.Read(0x2000) == 0x0a, "Missing or malformed PCM sets the audio error flag");
        chip.Write(0x2007, 3);
        Check(Tick(chip) == Sample{} && chip.Read(0x2000) == 0x0a,
              "Control writes cannot start an errored track");
    }
    for (uint16_t track : {5, 6, 7}) {
        Track(chip, track);
        Check(chip.Read(0x2000) == 2, "A valid header accepts empty and partial PCM payloads");
    }
    Play(chip, 6);
    Check(Tick(chip) == Sample{0x92, 0}, "A partial stereo sample pads missing bytes with zero");
    Play(chip, 7);
    Check(Tick(chip) == Sample{int16_t(-28108), 0x56},
          "Partial right channel pads only its missing byte");
    Play(chip, 1);
    Check(chip.Read(0x2000) == 0x12 && Tick(chip) == first[0],
          "A valid selection clears the error");
    chip.Write(0x2004, 4);
    Check(Tick(chip) == first[1], "A low track write leaves the open file running");
    chip.Write(0x2005, 1);
    chip.Write(0x2007, 1);
    Check(Tick(chip) == second[0], "The high write opens the full decimal track number");
}

void InputTicksVolumeAndMute() {
    Fixture fixture;
    fixture.Data();
    const std::array frames{Sample{-32768, 32767}, Sample{-12345, 23456}, Sample{123, -456}};
    fixture.Pcm(1, frames);
    Msu1 chip;
    Check(chip.Open(fixture.rom), "Open input-clock fixture");
    for (unsigned volume = 0; volume < 256; ++volume) {
        Play(chip, 1, 1, uint8_t(volume));
        for (const auto frame : frames) {
            const auto output = chip.Clock();
            for (unsigned side = 0; side < 2; ++side)
                Check(output[side] == float(double(frame[side]) / 32768 * volume / 255),
                      "Volume retains normalized float precision for both signed channels");
        }
        Check(chip.Read(0x2000) == 0x12 && Tick(chip) == Sample{} && chip.Read(0x2000) == 2,
              "EOF stops on the input tick following the last stereo frame");
    }
    Play(chip, 1);
    Check(Tick(chip, true) == Sample{} && Tick(chip) == frames[1],
          "DSP mute suppresses input while the file position advances");
    chip.Write(0x2007, 0);
    for (unsigned i = 0; i < 13; ++i)
        Check(Tick(chip) == Sample{}, "Paused ticks emit silence without consuming PCM");
    chip.Write(0x2007, 1);
    Check(Tick(chip) == frames[2], "Unpausing retains the next unread frame");
    chip.Write(0x2007, 2);
    Check(Tick(chip) == Sample{} && chip.Read(0x2000) == 0x22,
          "A paused repeat bit remains asserted through silent ticks");
}

void LoopBoundaryAndOffsetWidth() {
    Fixture fixture;
    fixture.Data();
    const std::array frames{Sample{100, -100}, Sample{200, -200}, Sample{300, -300}};
    fixture.Pcm(1, frames, 1);
    fixture.Pcm(2, frames, 3);
    fixture.Pcm(3, frames, 4);
    fixture.Pcm(4, frames, 0xffffffff);
    Msu1 chip;
    Check(chip.Open(fixture.rom), "Open loop fixture");
    Play(chip, 1, 3);
    for (const auto expected :
         {frames[0], frames[1], frames[2], Sample{}, frames[1], frames[2], Sample{}})
        Check(Tick(chip) == expected, "Each loop seek consumes exactly one silent input tick");
    Play(chip, 2, 3);
    for (const auto frame : frames)
        Check(Tick(chip) == frame, "An EOF loop offset permits the initial pass");
    for (unsigned i = 0; i < 8; ++i)
        Check(Tick(chip) == Sample{} && chip.Read(0x2000) == 0x32,
              "A loop offset equal to EOF stays silent with play and repeat set");
    Play(chip, 3, 3);
    for (const auto frame : frames)
        Check(Tick(chip) == frame, "An excessive loop offset permits the initial pass");
    Check(Tick(chip) == Sample{} && Tick(chip) == frames[0],
          "A loop offset beyond EOF falls back to the first PCM frame");
    Play(chip, 4, 3);
    for (const auto frame : frames)
        Check(Tick(chip) == frame, "A wrapped loop offset permits the initial pass");
    Check(Tick(chip) == Sample{} && Tick(chip) == Sample{-1, -1},
          "Loop byte offsets wrap to 32 bits before seeking");
}

void ResumeRegisterAndReset() {
    Fixture fixture;
    fixture.Data();
    const std::array first{Sample{111, -222}, Sample{333, -444}, Sample{555, -666}};
    const std::array second{Sample{-777, 888}, Sample{-999, 1111}, Sample{-1222, 1333}};
    for (uint16_t track : {0, 1, 65535})
        fixture.Pcm(track, first);
    fixture.Pcm(2, second);
    Msu1 chip;
    Check(chip.Open(fixture.rom), "Open resume fixture");
    for (uint16_t track : {0, 1, 65535}) {
        Play(chip, track);
        Check(Tick(chip) == first[0], "Each track starts at its first PCM frame after selection");
        chip.Write(0x2007, 4);
        Play(chip, 2);
        Check(Tick(chip) == second[0], "An unmatched track leaves the resume slot available");
        Play(chip, track);
        Check(Tick(chip) == first[1], "A matching selection restores its saved byte offset");
        Play(chip, track);
        Check(Tick(chip) == first[0], "A matching selection consumes the resume slot once");
    }
    Play(chip, 1);
    Tick(chip);
    chip.Write(0x2004, 2);
    chip.Write(0x2007, 4);
    Play(chip, 2);
    Check(Tick(chip) == second[1],
          "Resume records the current track register after a low-only write");
    chip.Write(0x2007, 4);
    chip.Reset();
    Check(chip.Read(0x2000) == 2 && chip.Clock() == Msu1::OutputFrame{},
          "Reset reopens track zero, clears volume and stops playback");
    Play(chip, 2);
    Check(Tick(chip) == second[0], "Reset discards the saved resume offset");
    chip.Close();
    Check(!chip.IsOpen() && chip.Clock() == Msu1::OutputFrame{},
          "Close removes playback and media");
}

} // namespace

int main() {
    try {
        DiscoveryMappingAndLifecycle();
        DataLatchesAndCacheBoundaries();
        SparseFourGigabyteData();
        TrackValidationAndSelection();
        InputTicksVolumeAndMute();
        LoopBoundaryAndOffsetWidth();
        ResumeRegisterAndReset();
        std::puts("MSU-1 register and input-clock tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
