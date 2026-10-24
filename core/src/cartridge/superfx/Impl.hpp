#pragma once
#include "snes/core/SuperFx.hpp"
#include "snes/core/Timing.hpp"
#include <array>
#include <vector>

namespace snes::core {
struct SuperFx::Impl {
    static constexpr uint16_t Zero = 0x0002, Carry = 0x0004, Sign = 0x0008, Overflow = 0x0010;
    static constexpr uint16_t Go = 0x0020, RomBusy = 0x0040, Alt1 = 0x0100, Alt2 = 0x0200;
    static constexpr uint16_t With = 0x1000, Irq = 0x8000, StatusMask = 0x9f7e;
    enum class Port : uint8_t { None, Rom, Ram };
    enum class Phase : uint8_t {
        Begin,
        Opcode,
        ImmediateLow,
        ImmediateHigh,
        Execute,
        Finish,
        StoreLow,
        StoreWordLow,
        StoreHigh,
        LoadLow,
        LoadWordLow,
        LoadHigh,
        RpixFlushFirst,
        RpixFlushSecond,
        RpixPlane,
        PlotEvict,
        PlotWrite,
        PlotFull,
        RomColor,
        RamBank,
        RomBank,
        RomByte
    };

    // These records retain only emulated values and named clock-return phases.
    struct Continuation {
        Phase phase = Phase::Begin;
        uint8_t opcode = 0;
        unsigned mode = 0;
        uint16_t input = 0, immediate = 0, operand = 0, word = 0;
        bool takeBranch = false, clearPrefix = true;
        uint32_t base = 0, address = 0;
        uint8_t data = 0, x = 0, y = 0;
        uint16_t row = 0;
        unsigned depth = 0, plane = 0;
    } work;

    struct FetchState {
        enum class Phase : uint8_t { Synchronize, Read, Access };
        uint16_t address = 0, offset = 0;
        uint8_t bank = 0;
        bool cached = false, hit = false;
        unsigned fillByte = 0;
        Phase phase = Phase::Read;
    } fetch;

    struct FlushState {
        unsigned index = 0;
        uint32_t address = 0;
        unsigned depth = 0, plane = 0;
        uint8_t data = 0;
        bool reading = false;
    } flush;

    struct PixelRow {
        uint16_t position = 0xffff;
        uint8_t pending = 0;
        std::array<uint8_t, 8> colors{};
    };

    // Cartridge storage stays bound to the target instance during restoration.
    std::span<const uint8_t> rom;
    std::span<uint8_t> ram;
    std::array<uint16_t, 16> r{};
    std::array<uint8_t, 512> cache{};
    uint32_t validLines = 0;
    std::array<PixelRow, 2> pixels{};
    uint16_t status = 0, cacheBase = 0, lastRam = 0;
    uint8_t programBank = 0, romBank = 0, ramBank = 0;
    uint8_t config = 0, screenBase = 0, screenMode = 0, clockSelect = 0;
    uint8_t backupEnable = 0, color = 0, plotOptions = 0, pipeline = 1;
    unsigned source = 0, destination = 0;
    bool pcWritten = false, r14Written = false, irqOutput = false;
    uint32_t romRemaining = 0, romAddress = 0;
    uint8_t romBuffer = 0;
    uint32_t ramRemaining = 0, ramAddress = 0;
    uint8_t ramBuffer = 0;
    uint32_t waitRemaining = 0;
    uint32_t masterFrequency = Timing::kMasterClockHz;
    int64_t masterBalance = 0;
    Port waitPort = Port::None;
    bool cacheFill = false, pixelFlush = false;
    enum class StepPhase : uint8_t { RomCountdown, RomAccess, RamCountdown, RamAccess, Clock };

    struct NativeStep {
        uint32_t clocks;
        StepPhase phase = StepPhase::RomCountdown;
        uint32_t address = 0;
        uint8_t data = 0;
    };

    std::vector<NativeStep> steps;
    bool idleStep = false, waitingForGo = true;

    Impl(std::span<const uint8_t> image, std::span<uint8_t> memory);
    inline bool Running() const;
    inline unsigned CoreCost() const;
    inline unsigned MemoryCost() const;
    inline bool Granted(Port port) const;

    static Port BankPort(uint8_t bank) {
        if (bank < 0x60)
            return Port::Rom;
        if (bank <= 0x7f)
            return Port::Ram;
        return Port::None;
    }

    inline uint8_t RamByte(uint32_t address) const;
    inline void StoreRam(uint32_t address, uint8_t value);
    inline uint8_t MemoryByte(uint32_t address) const;
    inline void Flag(uint16_t mask, bool set);
    inline void ResultFlags(uint16_t value, bool byteSign = false);
    inline void SetRegister(unsigned index, uint16_t value);
    inline void ClearPrefix();
    inline void StartRomRead();
    inline void SetColor(uint8_t value);
    inline unsigned Depth() const;
    inline uint32_t PixelAddress(uint8_t x, uint8_t y) const;
    static inline unsigned PlaneOffset(unsigned plane);
    inline void BeginFetch(uint16_t pc, Phase next);
    inline bool CompleteFetch();
    inline uint8_t FetchResult() const;
    inline bool BeginRamRead(uint32_t address, Phase next);
    inline bool BeginRamWrite(uint32_t address, uint8_t data, Phase next);
    inline void QueueRamWrite();
    inline bool BeginRomRead(Phase next);
    inline bool BeginDelay(uint32_t clocks, Port port, Phase next);
    inline bool BeginFlush(unsigned index, Phase next);
    inline void BeginPlane();
    inline bool CompletePixelAccess();
    inline void PrepareStep();
    inline void BeginWait(uint32_t clocks, Port port);
    inline void ResumeReady();
    inline void FinishStep();
    inline void Synchronize(bool inclusive);
    inline void Advance(uint64_t clocks);
    inline void AdvanceMasterClocks(uint32_t clocks);
    inline void Run();
    inline uint8_t ReadIo(uint16_t address);
    inline void WriteIo(uint16_t address, uint8_t value);
    inline bool CpuRamAddress(uint32_t address, uint32_t& offset) const;
    inline bool CpuRomAddress(uint32_t address, size_t& offset) const;
};
} // namespace snes::core
