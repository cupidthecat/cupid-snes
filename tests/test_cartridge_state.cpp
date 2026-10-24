#include "cartridge/StateFixture.hpp"
#include "snes/core/FilePath.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <cstdio>
#include <fstream>

using namespace snes::tests::cartridge_state;
using snes::core::detail::StateArchive;

namespace {
const char* stage = "startup";
unsigned position = 0;

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class Device> std::vector<uint8_t> Capture(Device& device) {
    StateArchive archive;
    device.SerializeState(archive);
    return archive.Bytes();
}

template <class Device> void Restore(Device& device, std::span<const uint8_t> bytes) {
    StateArchive archive(bytes);
    device.SerializeState(archive);
    archive.Commit();
}

template <class Device> void Reject(Device& device, std::span<const uint8_t> bytes) {
    const auto before = Capture(device);
    bool rejected = false;
    try {
        Restore(device, bytes);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, "Incompatible or malformed cartridge state must reject");
    Check(Capture(device) == before, "Rejected cartridge state retains live storage and execution");
}

template <class Device> void Truncation(Device& device) {
    const auto saved = Capture(device);
    for (size_t length = 0; length < saved.size(); length += std::max(size_t{1}, saved.size() / 7))
        Reject(device, std::span(saved).first(length));
    Reject(device, std::span(saved).first(saved.size() - 1));
    auto extra = saved;
    extra.push_back(0);
    Reject(device, extra);
}

void CanonicalEmptyPack() {
    for (Board board : {Board::BsxFlash, Board::SlotLo, Board::SlotHi, Board::SlotSa1}) {
        auto image = SlotBase(board);
        std::string error;
        auto direct = Require(Cartridge::FromRomImage(image, nullptr, nullptr, &error), error);
        auto empty = Require(Cartridge::FromBroadcastCartridge(image, {}, &error), error);
        Check(Capture(direct) == Capture(empty),
              "Equivalent empty pack loading paths produce the same hardware state");
    }
}

uint64_t Continuation() {
    uint64_t count = 0;
    for (unsigned profile = 0; profile < unsigned(Board::Count); ++profile)
        for (bool pal : {false, true}) {
            Fixture first(Board(profile), pal), second(Board(profile), pal);
            position = profile * 2 + unsigned(pal);
            Check(first.cart.BoardConnected() == (Board(profile) != Board::Disconnected),
                  "Generated board fixture has its intended physical connection");
            for (unsigned cut = 0; cut < 64; ++cut) {
                first.Edit(cut);
                const auto sourceTime = first.timeCalls, sourceAudio = first.audioCalls;
                const auto saved = Capture(first);
                Check(Capture(first) == saved && first.timeCalls == sourceTime &&
                          first.audioCalls == sourceAudio,
                      "Cartridge capture does not advance hardware or sample host callbacks");
                second.now = first.now;
                const auto calls = second.timeCalls, audio = second.audioCalls;
                Restore(second, saved);
                Check(second.timeCalls == calls && second.audioCalls == audio,
                      "Cartridge restore does not call host time or audio outputs");
                for (unsigned future : {cut + 1, cut + 7, cut + 31}) {
                    first.Edit(future);
                    second.Edit(future);
                    if (Capture(first) != Capture(second)) {
                        std::fprintf(stderr, "Board %u, region %u, cut %u, future %u\n", profile,
                                     unsigned(pal), cut, future);
                        throw std::runtime_error(
                            "Cartridge-owned storage and child execution must resume exactly");
                    }
                }
                Restore(first, saved);
                ++count;
            }
            Truncation(first);
            auto changed = Create(Board(profile), 1);
            changed.SetPal(pal);
            changed.SetSa1BusContention(false);
            Reject(changed, Capture(first.cart));
        }
    return count;
}

void Identity() {
    auto original = Image();
    std::vector<uint8_t> prefixed(512, 0x53);
    prefixed.insert(prefixed.end(), original.begin(), original.end());
    std::string error;
    auto first = Require(Cartridge::FromRomImage(original, nullptr, nullptr, &error), error);
    auto copier = Require(Cartridge::FromRomImage(prefixed, nullptr, nullptr, &error), error);
    first.SetMemselFast(true);
    first.Write(0x700123, 0x67);
    Restore(copier, Capture(first));
    Check(Capture(copier) == Capture(first),
          "Normalized copier headers do not change media identity");
    CartridgeDatabase database;
    DatabaseOverride entry;
    entry.forceTitle = "CUSTOM DISPLAY NAME";
    database.AddOverride(first.RomCrc32(), entry);
    auto renamed = Require(Cartridge::FromRomImage(original, nullptr, &database, &error), error);
    Restore(renamed, Capture(first));
    Check(Capture(renamed) == Capture(first),
          "A display-only title override preserves compatible hardware");
    entry.forceSramSize = 32768;
    database.AddOverride(first.RomCrc32(), entry);
    auto ram = Require(Cartridge::FromRomImage(original, nullptr, &database, &error), error);
    Reject(ram, Capture(first));
    entry.forceSramSize.reset();
    entry.forceMapping = MappingType::HiRom;
    database.AddOverride(first.RomCrc32(), entry);
    auto mapping = Require(Cartridge::FromRomImage(original, nullptr, &database, &error), error);
    Reject(mapping, Capture(first));
    for (auto pair : {std::pair{Board::Dsp1Command, Board::Dsp1Processor},
                      std::pair{Board::Dsp4Command, Board::Dsp4Processor},
                      std::pair{Board::Cx4Command, Board::Cx4Processor}}) {
        auto command = Create(pair.first), processor = Create(pair.second);
        Check(std::equal(command.RomData().begin(), command.RomData().end(),
                         processor.RomData().begin(), processor.RomData().end()),
              "Execution graph mismatch uses the same normalized ROM bytes");
        Reject(processor, Capture(command));
        Reject(command, Capture(processor));
    }
    // An invalid boolean follows the complete immutable identity and graph metadata.
    auto noRam = original;
    noRam[0x7fd8] = 0;
    auto empty = Require(Cartridge::FromRomImage(noRam, nullptr, nullptr, &error), error);
    const auto prefix = Capture(empty).size();
    auto invalid = Capture(first);
    invalid.at(prefix - 1) = 2;
    Reject(first, invalid);
    Cartridge unloaded, other;
    Restore(other, Capture(unloaded));
    Check(Capture(other) == Capture(unloaded), "An empty cartridge graph round-trips");
    Truncation(unloaded);
}

void SharedStorage() {
    std::string error;
    auto noRam = Image();
    noRam[0x7fd8] = 0;
    auto plain = Require(Cartridge::FromRomImage(noRam, nullptr, nullptr, &error), error);
    const auto prefix = Capture(plain).size();
    auto sa1 = Create(Board::Sa1);
    std::vector<uint8_t> ram(sa1.SramData().size());
    Sa1 core(sa1.RomData(), ram);
    Check(Capture(sa1).size() == prefix + Capture(core).size(),
          "SA-1 borrowed BWRAM appears exactly once in its cartridge state");
    auto fx = Create(Board::SuperFx);
    ram.resize(fx.SramData().size());
    SuperFx graphics(fx.RomData(), ram);
    Check(Capture(fx).size() == prefix + ram.size() + Capture(graphics).size(),
          "Super FX leaves exactly one shared RAM copy to its cartridge owner");
    auto command = Create(Board::St010Command), target = Create(Board::St010Command);
    Check(Capture(command).size() == prefix + 4096,
          "The stateless command model retains its hidden physical work RAM once");
    command.Write(0x680002, 0x93);
    Check(command.Read(0x680002) == 0x93, "The command fixture changes physical work RAM");
    Restore(target, Capture(command));
    Check(target.Read(0x680002) == command.Read(0x680002),
          "Command work RAM restores separately from the inactive battery image");
    for (Board board : {Board::Lo, Board::Hi, Board::Obc1, Board::Sdd1, Board::Sa1, Board::SuperFx,
                        Board::Spc7110, Board::St010Processor, Board::St011Processor,
                        Board::Competition92, Board::Competition94Processor, Board::Sufami}) {
        auto first = Create(board), second = Create(board);
        const auto* allocation = second.SramData().data();
        std::vector<uint8_t> bytes(first.SramData().size());
        Check(!bytes.empty(), "Shared storage fixture has physical RAM");
        for (size_t n = 0; n < bytes.size(); ++n)
            bytes[n] = uint8_t(n * 17 + n / 256 + 0x39);
        first.LoadSram(bytes);
        Restore(second, Capture(first));
        Check(second.SramData().data() == allocation &&
                  std::equal(bytes.begin(), bytes.end(), second.SramData().begin()),
              "Every mapped RAM byte restores into its original owner allocation");
    }
    Fixture pending(Board::Sa1), expected(Board::Sa1);
    pending.cart.AdvanceHardware(1, 0x808000, false, false);
    bool unprepared = false;
    try {
        (void)Capture(pending.cart);
    } catch (const std::runtime_error&) {
        unprepared = true;
    }
    Check(unprepared, "Unfinished SA-1 work requires explicit capture preparation");
    pending.cart.SynchronizeState();
    expected.cart.AdvanceHardware(1, 0x808000, false, false);
    expected.cart.SynchronizeState();
    Check(Capture(pending.cart) == Capture(expected.cart),
          "Capture rejection does not drain SA-1 work before explicit preparation");
}

void FlashIdentity() {
    for (Board board : {Board::BsxFlash, Board::SlotLo, Board::SlotHi, Board::SlotSa1}) {
        auto first = Create(board), second = Create(board), different = Create(board, 1);
        const auto* memory = second.MemoryPackData().data();
        std::vector<uint8_t> programmed(first.MemoryPackData().size(), 0x53);
        Check(first.LoadMemoryPack(programmed),
              "Replace current flash bytes before the first checkpoint");
        const auto saved = Capture(first);
        Restore(second, saved);
        Check(Capture(second) == saved && second.MemoryPackData().data() == memory &&
                  std::equal(programmed.begin(), programmed.end(), second.MemoryPackData().begin()),
              "Flash restore retains the allocation and its inserted-media identity");
        Check(different.LoadMemoryPack(programmed),
              "Give different inserted media identical current bytes");
        Reject(different, saved);
        second.Write(0xc05555, 0x40);
        second.Write(0xc01234, 0x26);
        Reject(second, std::span(saved).first(saved.size() - 1));
        Restore(second, saved);
        Check(Capture(second) == saved,
              "Changed flash command latches restore with their owner storage");
    }
    const auto bios = SlotBase(Board::BsxFlash);
    auto bytes = Pack(Board::BsxFlash);
    std::string error;
    auto writable = Require(Cartridge::FromBroadcastCartridge(bios, bytes, &error), error);
    bytes[0xff00] = 'M';
    bytes[0xff02] = 'P';
    bytes[0xff06] = 0x70;
    auto mask = Require(Cartridge::FromBroadcastCartridge(bios, bytes, &error), error);
    Check(Capture(writable).size() == Capture(mask).size() + bytes.size(),
          "Read-only pack bytes are identified as media rather than duplicated in state");
    Reject(mask, Capture(writable));
    Check(!mask.LoadMemoryPack(bytes),
          "Read-only media remains attached after rejected state input");
}

void MoveAndBindings() {
    for (Board board : {Board::Lo, Board::Sa1, Board::SuperFx, Board::BsxFlash, Board::SlotSa1,
                        Board::Handheld, Board::Competition92Processor}) {
        Fixture source(board), target(board);
        source.Edit(17);
        const auto saved = Capture(source.cart);
        const auto *rom = target.cart.RomData().data(), *ram = target.cart.SramData().data();
        const auto* pack = target.cart.MemoryPackData().data();
        Cartridge moved(std::move(target.cart));
        Restore(moved, saved);
        target.cart = std::move(moved);
        Check(target.cart.RomData().data() == rom && target.cart.SramData().data() == ram &&
                  target.cart.MemoryPackData().data() == pack,
              "Moving and restoring cartridge state retains every borrowed allocation");
        source.Edit(33);
        target.Edit(33);
        Check(Capture(source.cart) == Capture(target.cart),
              "Mapped children still use the target memory after owner moves");
    }
    Fixture source(Board::Handheld), target(Board::Handheld);
    Restore(target.cart, Capture(source.cart));
    const auto sourceCalls = source.timeCalls, targetCalls = target.timeCalls;
    target.cart.Reset();
    Check(source.timeCalls == sourceCalls && target.timeCalls > targetCalls,
          "Restored handheld time callbacks stay bound to their target owner");
}

void Portable(const char* path) {
    std::ofstream output(snes::core::FilePath(path), std::ios::binary);
    uint64_t size = 0;
    for (unsigned board = 0; board < unsigned(Board::Count); ++board)
        for (bool pal : {false, true}) {
            Fixture fixture(Board(board), pal);
            for (unsigned step = 0; step < 17; ++step)
                fixture.Edit(step);
            const auto bytes = Capture(fixture);
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         std::streamsize(bytes.size()));
            size += bytes.size();
        }
    Check(bool(output), "Write portable cartridge graph fixture");
    std::printf("Portable cartridge graph fixture: %llu bytes\n", (unsigned long long)size);
}
} // namespace

int main(int argc, char** argv) {
    try {
        CanonicalEmptyPack();
        stage = "identity";
        Identity();
        stage = "shared storage";
        SharedStorage();
        stage = "flash identity";
        FlashIdentity();
        stage = "moves and bindings";
        MoveAndBindings();
        stage = "continuation";
        const auto count = Continuation();
        if (argc == 2)
            Portable(argv[1]);
        std::printf("Cartridge owner state: %llu continuation checks passed\n",
                    (unsigned long long)count);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s/%u: %s\n", stage, position, error.what());
        return 1;
    }
}
