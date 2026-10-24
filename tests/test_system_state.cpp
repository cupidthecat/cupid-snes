#include "snes/core/AutoJoypad.hpp"
#include "snes/core/BroadcastReceiver.hpp"
#include "snes/core/CpuIoRegisters.hpp"
#include "snes/core/Dma.hpp"
#include "snes/core/IrqController.hpp"
#include "snes/core/MemoryBus.hpp"
#include "snes/core/Timing.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace snes::core;
using snes::core::detail::StateArchive;

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <class Operation> void Reject(Operation operation, const char* message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Check(rejected, message);
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

template <class Device> void RejectionIsAtomic(Device& device, std::vector<uint8_t> bytes) {
    const auto before = Capture(device);
    Reject([&] { Restore(device, bytes); }, "Malformed state must reject");
    Check(Capture(device) == before, "Rejected state preserves every live field");
}

template <class Device> void TruncationIsAtomic(Device& device) {
    const auto bytes = Capture(device);
    for (size_t prefix = 0; prefix < bytes.size(); prefix += bytes.size() > 1024 ? 997 : 1)
        RejectionIsAtomic(device, {bytes.begin(), bytes.begin() + prefix});
    auto extra = bytes;
    extra.push_back(0);
    RejectionIsAtomic(device, std::move(extra));
}

void Digests() {
    const auto digest = [](std::string_view text) {
        const auto bytes =
            detail::Sha256({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        std::string hex;
        for (auto byte : bytes) {
            hex += "0123456789abcdef"[byte >> 4];
            hex += "0123456789abcdef"[byte & 15];
        }
        return hex;
    };
    Check(digest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "Empty SHA-256 vector");
    Check(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "Short SHA-256 vector");
    Check(digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "SHA-256 padding can require another block");
    Check(digest(std::string(1000000, 'a')) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "Long SHA-256 vector");
}

void BusContinuation() {
    auto first = std::make_unique<MemoryBus>(), second = std::make_unique<MemoryBus>();
    unsigned observed = 0;
    second->Map(
        0, 0, 0x5000, 0x5000, [&](uint32_t, uint8_t) { return uint8_t(++observed); },
        [](uint32_t, uint8_t) {});
    first->MapWram();
    second->MapWram();
    auto* memory = second->WramData();
    for (size_t index = 0; index < MemoryBus::WramSize; ++index)
        first->WramData()[index] = uint8_t(index * 67 + (index >> 8));
    first->Write(0x2181, 0xfe);
    first->Write(0x2182, 0xff);
    first->Write(0x2183, 1);
    first->SetFastRom(true);
    first->SetOpenBus(0xc7);
    const auto bytes = Capture(*first);
    Restore(*second, bytes);
    Check(second->WramData() == memory && second->WmdataAddress() == 0x1fffe &&
              second->OpenBus() == 0xc7 && second->Speed(0xc08000) == 6,
          "Bus state restores in place with WMDATA, MDR and fast ROM");
    Check(observed == 0 && second->Read(0x005000) == 1,
          "Restore preserves target handler slots without invoking them");
    second->SetOpenBus(first->OpenBus());
    for (unsigned step = 0; step < 40; ++step) {
        Check(first->Read(0x2180) == second->Read(0x2180), "WMDATA reads resume across bank wrap");
        first->Write(0x2180, uint8_t(step * 11));
        second->Write(0x2180, uint8_t(step * 11));
        Check(first->Read(0x7e0000 + step) == second->Read(0x7e0000 + step), "WRAM reads resume");
    }
    Check(Capture(*first) == Capture(*second), "Bus continuation retains all physical WRAM");
    TruncationIsAtomic(*second);
    auto bad = bytes;
    bad[MemoryBus::WramSize + 2] = 2;
    RejectionIsAtomic(*second, bad);
    bad = bytes;
    bad.back() = 2;
    RejectionIsAtomic(*second, bad);
}

void IoContinuation() {
    for (uint8_t revision : {1, 2})
        for (bool divide : {false, true})
            for (unsigned cut = 0; cut <= (divide ? 16u : 8u); ++cut) {
                CpuIoRegisters first, second;
                first.setCpuVersion(revision);
                second.setCpuVersion(revision);
                unsigned calls = 0;
                second.SetPioCallback([&](uint8_t) { ++calls; });
                second.SetMemselCallback([&](bool) { ++calls; });
                second.SetNmitimenCallback([&](uint8_t) { ++calls; });
                second.SetHVTimeChangeCallback([&](uint16_t, uint16_t) { ++calls; });
                second.SetJoypadLatchCallback([&](bool) { ++calls; });
                first.Write(0x4200, 0xb1);
                first.Write(0x4201, 0x37);
                first.Write(0x4207, 0x39);
                first.Write(0x4208, 0);
                first.Write(0x4209, 0xef);
                first.Write(0x420a, 0);
                first.Write(0x420d, 1);
                first.Write(0x4016, 1);
                first.setJoy1(0x8750);
                first.setJoy2(0x1640);
                first.setJoy3(0x8ea0);
                first.setJoy4(0x0310);
                first.setAutoJoypadCounter(17);
                first.setNmiFlag(true);
                first.setIrqFlag(true);
                if (divide) {
                    first.Write(0x4204, 0xe7);
                    first.Write(0x4205, 0x91);
                    first.Write(0x4206, 0x39);
                } else {
                    first.Write(0x4202, 0xb7);
                    first.Write(0x4203, 0x91);
                }
                for (unsigned step = 0; step < cut; ++step)
                    first.AluStep(step == cut - 1);
                const auto bytes = Capture(first);
                Restore(second, bytes);
                Check(calls == 0, "CPU I/O restore does not replay writes or trigger callbacks");
                for (unsigned step = 0; step < 24; ++step) {
                    Check(first.rddiv() == second.rddiv() && first.rdmpy() == second.rdmpy(),
                          "Partial multiply/divide retains each intermediate result");
                    for (uint32_t address = 0x4210; address <= 0x421f; ++address)
                        Check(first.Read(address, 0x6d) == second.Read(address, 0x6d),
                              "CPU I/O read-and-clear continuation");
                    if (step == 0) {
                        first.Write(divide ? 0x4206 : 0x4203, 0x1b);
                        second.Write(divide ? 0x4206 : 0x4203, 0x1b);
                    }
                    first.AluStep(step % 3 == 0);
                    second.AluStep(step % 3 == 0);
                }
                Check(Capture(first) == Capture(second),
                      "CPU I/O retains the sampled write-busy signal");
                second.Write(0x4201, 0xff);
                second.Write(0x420d, 0);
                second.Write(0x4016, 0);
                second.Write(0x4200, 0);
                second.Write(0x4207, 0);
                Check(calls == 5, "CPU I/O host callbacks remain attached after restore");
                if (cut == 3 && divide) {
                    TruncationIsAtomic(second);
                    auto bad = bytes;
                    bad[4] = 0;
                    RejectionIsAtomic(second, bad);
                    bad = bytes;
                    bad[32] = 1;
                    RejectionIsAtomic(second, bad);
                    second.setCpuVersion(uint8_t(3 - revision));
                    RejectionIsAtomic(second, bytes);
                }
            }
}

struct TimingRig {
    Timing timing;
    IrqController irq;
    using Event = std::array<uint64_t, 5>;
    std::vector<Event> events;

    explicit TimingRig(Region region = Region::NTSC, uint8_t revision = 2)
        : timing(region, revision) {
        timing.onFrameBegin = [&] { EventAt(0); };
        timing.onScanline = [&](uint16_t line) { EventAt(1, line); };
        timing.onVBlankBegin = [&] { EventAt(2); };
        timing.onHdmaSetup = [&] { EventAt(3); };
        timing.onNmiPoint = [&] { EventAt(4); };
        timing.onRenderCycle = [&](uint16_t line) { EventAt(5, line); };
        timing.onDramRefresh = [&] { EventAt(6); };
        timing.onHBlank = [&](uint16_t line) { EventAt(7, line); };
        timing.onHdmaTransfer = [&](uint16_t line) { EventAt(8, line); };
        timing.onIrqPoll = [&](uint16_t h, uint16_t v, uint16_t vdisp, uint16_t hperiod) {
            irq.Poll(h, v, vdisp, hperiod, timing.PreviousVPeriod());
            EventAt(9, irq.NmiLine() | (irq.NmiHold() << 1) | (irq.IrqLine() << 2) |
                           (irq.IrqHold() << 3) | (irq.NmiTransition() << 4) |
                           (irq.IrqTransition() << 5));
        };
        timing.onJoypadPoll = [&](uint16_t, uint16_t, uint16_t) { EventAt(10); };
        irq.NmitimenUpdate(0xb0);
        irq.SetHTime(137);
        irq.SetVTime(225);
    }

    void EventAt(uint64_t kind, uint64_t value = 0) {
        events.push_back(
            {kind, timing.MasterClocksElapsed(), timing.HCounter(), timing.VCounter(), value});
    }

    void SerializeState(StateArchive& archive) {
        timing.SerializeState(archive);
        irq.SerializeState(archive);
    }
};

void TimingContinuation() {
    for (auto region : {Region::NTSC, Region::PAL})
        for (uint8_t revision : {1, 2})
            for (bool interlace : {false, true}) {
                const uint32_t field = (region == Region::NTSC ? 262u : 312u) * 1364;
                for (uint32_t cut :
                     {0u, 1u, 13u, 511u, 537u, 1099u, 1104u, 1363u, 128u * 1364 + 1,
                      225u * 1364 + 301, field + 240 * 1364 + 1359, field + 311 * 1364 + 1361}) {
                    TimingRig first(region, revision), second(region, revision);
                    first.timing.SetInterlace(interlace);
                    first.timing.SetVDisp(240);
                    first.timing.Tick(cut);
                    first.timing.SetInterlace(!interlace);
                    if (first.timing.VCounter() == 225)
                        first.timing.SetVDisp(225);
                    const auto bytes = Capture(first);
                    Restore(second, bytes);
                    Check(second.events.empty(), "Timing restore fires no hardware event");
                    first.events.clear();
                    for (auto clocks : {1u, 3u, 2u, 127u, 129u, 1097u, field * 2 + 17}) {
                        first.timing.Tick(clocks);
                        second.timing.Tick(clocks);
                        Check(
                            first.irq.Rdnmi() == second.irq.Rdnmi() &&
                                first.irq.Timeup() == second.irq.Timeup() &&
                                first.irq.NmiTest() == second.irq.NmiTest() &&
                                first.irq.IrqTest(true) == second.irq.IrqTest(true),
                            "Interrupt holds, dispatch and read-and-clear resume on the same edge");
                    }
                    Check(first.events == second.events,
                          "Every timing callback resumes at the same H/V clock");
                    Check(Capture(first) == Capture(second),
                          "Timing continuation retains all event and divider phases");
                }
            }
    TimingRig first, second;
    first.timing.Tick(225 * 1364 + 2);
    Check(first.irq.NmiHold() && first.irq.Rdnmi() && first.irq.Rdnmi(),
          "Checkpoint fixture reaches the NMI read-protection window");
    Restore(second, Capture(first));
    Check(second.irq.NmiHold() && second.irq.Rdnmi() && second.irq.Rdnmi(),
          "Restore retains NMI hold");
    first.irq.SetHTime(0);
    first.irq.SetVTime(225);
    first.irq.PollTimer(14, 225, 1364);
    Check(first.irq.IrqHold() && first.irq.Timeup() && first.irq.Timeup(),
          "Checkpoint fixture reaches the timer read-protection window");
    Restore(second, Capture(first));
    Check(second.irq.IrqHold() && second.irq.Timeup() && second.irq.Timeup(),
          "Restore retains IRQ hold");
    TruncationIsAtomic(second);
    for (const auto offset : {size_t{9}, size_t{13}, size_t{22}, size_t{44}}) {
        auto bad = Capture(first);
        bad[offset] = 0xff;
        RejectionIsAtomic(second, bad);
    }
    auto badIrq = Capture(first.irq);
    badIrq[7] = 0;
    RejectionIsAtomic(second.irq, badIrq);
    badIrq = Capture(first.irq);
    badIrq[14] ^= 4;
    RejectionIsAtomic(second.irq, badIrq);
    TimingRig otherRegion(Region::PAL), otherRevision(Region::NTSC, 1);
    RejectionIsAtomic(otherRegion, Capture(first));
    RejectionIsAtomic(otherRevision, Capture(first));
    second.timing.SetRenderCycle(1100);
    RejectionIsAtomic(second, Capture(first));
}

struct DmaRig {
    std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
    DmaController dma;
    uint32_t address = 0x808000;
    uint8_t data = 0x6d;
    uint64_t elapsed = 3;
    std::vector<std::array<uint64_t, 4>> events;

    DmaRig() {
        bus->MapWram();
        bus->Map(
            0, 0, 0x2100, 0x217f,
            [&](uint32_t addr, uint8_t mdr) {
                events.push_back({0, elapsed, addr, mdr});
                return uint8_t(addr * 7 + elapsed);
            },
            [&](uint32_t addr, uint8_t value) { events.push_back({1, elapsed, addr, value}); });
        dma.SetBus(bus.get());
        dma.SetCpuBusLatches(address, data);
        dma.SetClockQuery([&] { return elapsed; });
        dma.SetClockCallback([&](uint32_t clocks) { elapsed += clocks; });
        dma.SetCoprocessorSyncCallback([&] { events.push_back({2, elapsed, address, data}); });
        dma.SetBoundaryCallback([&] { events.push_back({3, elapsed, address, data}); });
    }

    void Configure(unsigned mode, bool indirect) {
        for (size_t index = 0; index < MemoryBus::WramSize; ++index)
            bus->WramData()[index] = uint8_t(index * 71 + (index >> 5));
        for (unsigned index = 0; index < 8; ++index) {
            auto& channel = dma.Channel(index);
            channel.writeControl(uint8_t(mode | (indirect ? 0x40 : 0) | (index & 1 ? 0xa0 : 0)));
            channel.sourceBank = channel.indirectBank = 0x7e;
            channel.sourceAddress = uint16_t(0x1000 + index * 0x100);
            channel.targetAddress = uint8_t(0x10 + index * 4);
            channel.unknown = uint8_t(0xa0 + index);
            std::vector<uint8_t> table;
            if (indirect) {
                table = {0x83, 0,    uint8_t(0x40 + index), 0x02, 0x40, uint8_t(0x40 + index),
                         0x81, 0x80, uint8_t(0x40 + index), 0,    0xc0, uint8_t(0x40 + index)};
            } else {
                for (auto [lines, transfers] : {std::pair{0x83, 3}, {0x02, 1}, {0x81, 1}}) {
                    table.push_back(uint8_t(lines));
                    for (unsigned byte = 0;
                         byte < unsigned(transfers) * DmaController::kTransferLengths[mode]; ++byte)
                        table.push_back(uint8_t(byte * 11 + index));
                }
                table.push_back(0);
            }
            std::copy(table.begin(), table.end(), bus->WramData() + channel.sourceAddress);
        }
        dma.EnableHdma(0xff);
    }

    void SerializeState(StateArchive& archive) {
        bus->SerializeState(archive);
        dma.SerializeState(archive);
        archive.Number(address);
        archive.Number(data);
        archive.Number(elapsed);
    }
};

void DmaContinuation() {
    for (unsigned mode = 0; mode < 8; ++mode)
        for (bool indirect : {false, true})
            for (unsigned cut : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u}) {
                DmaRig first, second;
                first.Configure(mode, indirect);
                first.dma.SetCpuCycleClocks(mode % 3 == 0 ? 6 : mode % 3 == 1 ? 8 : 12);
                first.dma.HdmaReset();
                first.dma.HdmaSetup();
                for (unsigned line = 0; line < cut; ++line)
                    first.dma.HdmaRun();
                Restore(second, Capture(first));
                Check(second.dma.Bus() == second.bus.get() && second.events.empty(),
                      "DMA restore retains the target bus and never transfers data");
                first.events.clear();
                for (unsigned line = cut; line < 12; ++line)
                    Check(first.dma.HdmaRun() == second.dma.HdmaRun(),
                          "HDMA continuation consumes identical clocks");
                Check(first.events == second.events,
                      "HDMA resumes exact data, table fetches and host latch callbacks");
                Check(Capture(first) == Capture(second),
                      "HDMA retains each channel and indirect/repeat phase");
            }
    for (unsigned mode = 0; mode < 8; ++mode) {
        DmaRig first, second;
        for (unsigned index = 0; index < 8; ++index) {
            auto& channel = first.dma.Channel(index);
            channel.writeControl(uint8_t(mode | ((index & 1) ? 0x80 : 0) |
                                         ((index & 2) ? 0x10 : 0) | ((index & 4) ? 0x08 : 0)));
            channel.sourceBank = 0x7e;
            channel.sourceAddress = uint16_t(index * 97);
            channel.targetAddress = uint8_t(index * 4);
            channel.transferSize = uint16_t(index + 1);
        }
        first.dma.EnableDma(0xff);
        Restore(second, Capture(first));
        Check(first.dma.RunDma() == second.dma.RunDma(),
              "Pending GP-DMA resumes all transfer modes");
        Check(first.events == second.events && Capture(first) == Capture(second),
              "GP-DMA retains direction, fixed/reverse addressing and CPU bus latches");
    }
    DmaRig rig;
    rig.Configure(1, true);
    TruncationIsAtomic(rig.dma);
    auto bad = Capture(rig.dma);
    bad[0] = 8;
    RejectionIsAtomic(rig.dma, bad);
    bad = Capture(rig.dma);
    bad.back() = 1;
    RejectionIsAtomic(rig.dma, bad);
    bool checkedDma = false, checkedHdma = false;
    rig.dma.SetClockCallback([&](uint32_t clocks) {
        rig.elapsed += clocks;
        if (rig.dma.InDma()) {
            checkedDma = true;
            Reject([&] { Capture(rig.dma); }, "DMA capture rejects a retained transfer stack");
        }
        if (rig.dma.InHdma()) {
            checkedHdma = true;
            Reject([&] { Capture(rig.dma); }, "HDMA capture rejects a retained transfer stack");
        }
    });
    rig.dma.HdmaSetup();
    rig.dma.Channel(0).transferSize = 1;
    rig.dma.EnableDma(1);
    rig.dma.RunDma();
    Check(checkedDma && checkedHdma, "Both transfer engines enforce portable capture boundaries");
}

struct InputRig {
    AutoJoypad joy;
    unsigned samples = 0, latches = 0, frame = 0;

    InputRig() {
        joy.SetInputCallback([&](int player) {
            ++samples;
            InputState input;
            input.b = ((player + frame) & 1) != 0;
            input.y = (player & 2) != 0;
            input.a = input.start = true;
            input.left = (player & 4) != 0;
            input.r = !input.b;
            return input;
        });
        joy.Ports().SetMouseCallback([&](int port) {
            ++samples;
            return MouseState{int32_t(frame * 17 - 31), int32_t(port * 7 + 29), true,
                              (frame & 1) != 0};
        });
        joy.Ports().SetGunCallback([&](int port, int gun) {
            ++samples;
            return LightGunState{int32_t(100 + gun * 11 + frame),
                                 int32_t(100 + port),
                                 true,
                                 true,
                                 (frame & 1) != 0,
                                 true,
                                 true,
                                 frame == 3};
        });
        joy.Ports().SetGunLatchCallback([&](uint16_t, uint16_t) { ++latches; });
    }

    void Configure(ControllerDevice device) {
        joy.Ports().Configure(0, device, {7, 6, 5, 4});
        joy.Ports().Configure(1, device, {3, 2, 1, 0});
        joy.Ports().SetVDisp(240);
    }

    void SerializeState(StateArchive& archive) {
        joy.SerializeState(archive);
    }
};

void InputContinuation() {
    for (auto device :
         {ControllerDevice::None, ControllerDevice::Gamepad, ControllerDevice::Mouse,
          ControllerDevice::Multitap, ControllerDevice::SuperScope, ControllerDevice::Justifier,
          ControllerDevice::Justifiers, ControllerDevice::MacsRifle})
        for (bool high : {false, true})
            for (unsigned cut : {0u, 1u, 5u, 8u, 16u, 24u, 31u, 32u, 36u}) {
                InputRig first, second;
                first.Configure(device);
                second.Configure(ControllerDevice::None);
                first.joy.SetManualLatch(true);
                first.joy.SetManualLatch(high);
                first.joy.Ports().LatchLightGuns();
                for (unsigned step = 0; step < cut; ++step) {
                    first.joy.Ports().Read(0);
                    first.joy.Ports().Read(1);
                }
                first.frame = second.frame = 1;
                Restore(second, Capture(first));
                Check(second.samples == 0 && second.latches == 0,
                      "Input restore neither samples host devices nor strobes the counter latch");
                first.samples = first.latches = 0;
                for (unsigned step = 0; step < 80; ++step) {
                    if (step % 11 == 0) {
                        first.joy.Ports().SetPio(uint8_t(step & 1 ? 0 : 0xff));
                        second.joy.Ports().SetPio(uint8_t(step & 1 ? 0 : 0xff));
                    }
                    if (step == 19 || step == 40 || step == 41) {
                        first.joy.SetManualLatch(step != 40);
                        second.joy.SetManualLatch(step != 40);
                    }
                    if (step % 7 == 0) {
                        ++first.frame;
                        ++second.frame;
                        first.joy.Ports().LatchLightGuns();
                        second.joy.Ports().LatchLightGuns();
                    }
                    Check(first.joy.Ports().Read(0) == second.joy.Ports().Read(0) &&
                              first.joy.Ports().Read(1) == second.joy.Ports().Read(1),
                          "Controller packets, sensitivity, pair selection and button locks "
                          "continue exactly");
                }
                Check(first.samples == second.samples && first.latches == second.latches &&
                          Capture(first) == Capture(second),
                      "Restored controller hardware retains target callbacks and input sampling "
                      "cadence");
            }
    for (unsigned cut = 0; cut <= 33; ++cut)
        for (bool manual : {false, true}) {
            InputRig first, second;
            first.Configure(ControllerDevice::Multitap);
            first.joy.SetManualLatch(manual);
            first.joy.SetAutoJoypadPoll(true);
            first.joy.Tick128(256, 225, 225, true);
            for (unsigned step = 0; step < cut; ++step)
                first.joy.Tick128(512, 226, 225, (step & 1) == 0);
            Restore(second, Capture(first));
            first.samples = second.samples = 0;
            for (unsigned step = cut; step < 40; ++step) {
                if (step == 8) {
                    first.joy.SetAutoJoypadPoll(false);
                    second.joy.SetAutoJoypadPoll(false);
                }
                first.joy.Tick128(512, 226, 225, (step & 1) == 0);
                second.joy.Tick128(512, 226, 225, (step & 1) == 0);
                Check(first.joy.Joy1() == second.joy.Joy1() &&
                          first.joy.Joy2() == second.joy.Joy2() &&
                          first.joy.Joy3() == second.joy.Joy3() &&
                          first.joy.Joy4() == second.joy.Joy4() &&
                          first.joy.Counter() == second.joy.Counter(),
                      "Automatic polling resumes its sampled wire bits and half-shift phase");
            }
            Check(first.samples == second.samples && Capture(first) == Capture(second),
                  "Manual and automatic latch ownership survive restore");
        }
    InputRig first, second;
    first.Configure(ControllerDevice::MacsRifle);
    first.joy.Ports().PollLightGuns();
    first.joy.Ports().BeamPosition(176, 143, 225);
    Check(first.joy.Ports().IoBits() == 0x7f, "Rifle fixture reaches its beam pulse");
    Restore(second, Capture(first));
    Check(second.joy.Ports().IoBits() == 0x7f && second.joy.Ports().NeedsBeamPosition(),
          "A restored rifle pulse still needs its release edge");
    first.joy.Ports().BeamPosition(177, 143, 225);
    second.joy.Ports().BeamPosition(177, 143, 225);
    Check(second.joy.Ports().IoBits() == 0xff && Capture(first) == Capture(second),
          "The restored beam pulse releases once");
    TruncationIsAtomic(second);
    auto bad = Capture(second);
    bad[2] = 0xff;
    RejectionIsAtomic(second, bad);
    for (auto [offset, value] : {std::pair{size_t{30}, uint8_t{33}},
                                 {size_t{42}, uint8_t{3}},
                                 {bad.size() - 10, uint8_t{4}}}) {
        bad = Capture(second);
        bad[offset] = value;
        RejectionIsAtomic(second, bad);
    }
}

void Tune(BroadcastReceiver& receiver, unsigned stream, uint16_t channel) {
    const auto base = 0x2188 + stream * 6;
    receiver.WriteCpu(base, uint8_t(channel));
    receiver.WriteCpu(base + 1, uint8_t(channel >> 8));
    receiver.WriteCpu(base + 3, 1);
    receiver.WriteCpu(base + 4, 1);
}

void Streams(BroadcastReceiver& receiver, unsigned salt = 0) {
    for (uint8_t sequence = 0; sequence < 2; ++sequence) {
        std::vector<uint8_t> asset(22 * 7 + 3);
        for (size_t index = 0; index < asset.size(); ++index)
            asset[index] = uint8_t(index * 41 + sequence * 7 + salt);
        Check(receiver.LoadStream(0x1234, sequence, asset), "Reception fixture loads");
    }
}

void BroadcastContinuation() {
    for (bool recorded : {false, true})
        for (unsigned cut = 0; cut < 23; ++cut) {
            BroadcastReceiver first, second;
            unsigned firstSamples = 0, secondSamples = 0;
            first.SetTimeSource([&] {
                ++firstSamples;
                return BroadcastTime{2041, 11, 27, 4, 21, 39, 57};
            });
            second.SetTimeSource([&] {
                ++secondSamples;
                return BroadcastTime{2041, 11, 27, 4, 21, 39, 57};
            });
            if (recorded) {
                Streams(first);
                Streams(second);
                Tune(first, 0, 0);
                Tune(first, 1, 0x1234);
                first.ReadCpu(0x2190, 0);
                first.ReadCpu(0x2191, 0);
                for (unsigned byte = 0; byte < cut; ++byte)
                    first.ReadCpu(0x2192, 0);
            } else {
                first.WriteCpu(0x2188, 0x39);
                first.WriteCpu(0x2189, 0xc7);
                first.WriteCpu(0x2193, 0xfd);
            }
            const uint32_t clock = recorded ? 0x218c : 0x2192;
            for (unsigned byte = 0; byte < cut; ++byte)
                first.ReadCpu(clock, 0);
            Restore(second, Capture(first));
            Check(secondSamples == 0, "Broadcast restore never samples the host clock");
            firstSamples = 0;
            for (unsigned step = 0; step < 700; ++step) {
                Check(first.ReadCpu(clock, 0xa7) == second.ReadCpu(clock, 0xa7),
                      "Partial serial calendar resumes without resampling its prefix");
                if (recorded)
                    for (auto reg : {0x2190u, 0x2191u, 0x2192u, 0x2193u})
                        Check(first.ReadCpu(reg, 0xa7) == second.ReadCpu(reg, 0xa7),
                              "Recorded reception retains prefix count, data position and status "
                              "clears");
            }
            Check(firstSamples == secondSamples && firstSamples > 0 &&
                      Capture(first) == Capture(second),
                  "Broadcast callbacks remain attached after the saved calendar record finishes");
            if (cut == 17) {
                TruncationIsAtomic(second);
                auto bad = Capture(second);
                bad.back() = 0xff;
                RejectionIsAtomic(second, bad);
                if (recorded) {
                    bad = Capture(second);
                    // Two asset identities, radio registers, first receiver and second data cursor.
                    std::fill_n(bad.begin() + 5 + 2 * 39 + 24 + 16 + 8, 4, uint8_t{0xff});
                    RejectionIsAtomic(second, bad);
                    BroadcastReceiver changed;
                    Streams(changed, 1);
                    RejectionIsAtomic(changed, Capture(first));
                    changed.ClearStreams();
                    RejectionIsAtomic(changed, Capture(first));
                }
            }
        }
}

std::vector<uint8_t> PortableFixture() {
    DmaRig dma;
    dma.Configure(7, true);
    dma.dma.HdmaSetup();
    dma.dma.HdmaRun();
    TimingRig timing(Region::PAL, 1);
    timing.timing.SetInterlace(true);
    timing.timing.Tick(225 * 1364 + 301);
    InputRig input;
    input.Configure(ControllerDevice::Mouse);
    input.joy.SetAutoJoypadPoll(true);
    input.joy.Tick128(256, 225, 225, true);
    for (unsigned step = 0; step < 18; ++step)
        input.joy.Tick128(512, 226, 225, false);
    CpuIoRegisters io;
    io.Write(0x4204, 0xe7);
    io.Write(0x4205, 0x91);
    io.Write(0x4206, 0x39);
    io.AluStep(true);
    BroadcastReceiver receiver;
    Streams(receiver);
    receiver.SetTimeSource([] { return BroadcastTime{2041, 11, 27, 4, 21, 39, 57}; });
    Tune(receiver, 0, 0);
    Tune(receiver, 1, 0x1234);
    receiver.ReadCpu(0x2190, 0);
    receiver.ReadCpu(0x2191, 0);
    for (unsigned step = 0; step < 17; ++step) {
        receiver.ReadCpu(0x218c, 0);
        receiver.ReadCpu(0x2192, 0);
    }
    StateArchive archive;
    for (unsigned pass = 0; pass < 2; ++pass) {
        dma.SerializeState(archive);
        timing.SerializeState(archive);
        input.SerializeState(archive);
        io.SerializeState(archive);
        receiver.SerializeState(archive);
        dma.dma.HdmaRun();
        timing.timing.Tick(426003);
        input.joy.Tick128(512, 226, 225, false);
        io.AluStep();
        receiver.ReadCpu(0x218c, 0);
        receiver.ReadCpu(0x2192, 0);
    }
    return archive.Bytes();
}
} // namespace

int main() {
    try {
        Digests();
        BusContinuation();
        IoContinuation();
        TimingContinuation();
        DmaContinuation();
        InputContinuation();
        BroadcastContinuation();
        Check(!PortableFixture().empty(), "Portable system fixture captures each device");
        std::puts(
            "Bus, arithmetic, timing, DMA, interrupt, controller and broadcast checkpoint checks "
            "passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
