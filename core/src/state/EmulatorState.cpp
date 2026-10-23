#include "snes/core/Emulator.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/detail/Sha256.hpp"
#include "snes/core/detail/StateArchive.hpp"
#include "snes/core/detail/StateDigest.hpp"
#include "snes/core/gameboy/StartupState.hpp"

#include <stdexcept>
#include <utility>

namespace snes::core {
namespace {
constexpr std::array<uint8_t, 8> kMagic{'C', 'U', 'P', 'I', 'D', 'S', 'T', 0};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 48;
using detail::StateArchive;

template <class Fields> void Input(Fields& fields, InputState& input) {
    fields.Number(input.up);
    fields.Number(input.down);
    fields.Number(input.left);
    fields.Number(input.right);
    fields.Number(input.a);
    fields.Number(input.b);
    fields.Number(input.x);
    fields.Number(input.y);
    fields.Number(input.l);
    fields.Number(input.r);
    fields.Number(input.start);
    fields.Number(input.select);
}
} // namespace

struct Emulator::PreparedStateRestore::Impl {
    std::weak_ptr<uint8_t> lifetime;
    std::vector<uint8_t> bytes;
    std::unique_ptr<StateArchive> archive;
};

Emulator::PreparedStateRestore::PreparedStateRestore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

Emulator::PreparedStateRestore::PreparedStateRestore(PreparedStateRestore&&) noexcept = default;
Emulator::PreparedStateRestore&
Emulator::PreparedStateRestore::operator=(PreparedStateRestore&&) noexcept = default;
Emulator::PreparedStateRestore::~PreparedStateRestore() = default;

void Emulator::PreparedStateRestore::Commit() {
    if (!impl_ || impl_->lifetime.expired())
        throw std::runtime_error("Prepared machine state is no longer attached to this hardware");
    impl_->archive->Commit();
    impl_.reset();
}

void Emulator::InvalidatePreparedRestores() noexcept {
    restoreLifetime_.reset();
}

std::vector<uint8_t> Emulator::CaptureState(const StateOperation& operation) {
    operation.Report(StatePhase::Synchronize);
    SynchronizeState(operation);
    StateArchive payload;
    payload.SetProgress(operation);
    SerializeState(payload);
    if (payload.Bytes().size() > StateArchive::MaximumBytes - kHeaderBytes)
        throw std::runtime_error("Machine state exceeds the supported size");
    StateArchive header;
    for (auto byte : kMagic)
        header.Expect(byte);
    header.Expect(kVersion);
    header.Expect(uint32_t(payload.Bytes().size()));
    for (auto byte : detail::StateDigest(payload.Bytes(), operation, StatePhase::Capture))
        header.Expect(byte);
    auto bytes = header.Bytes();
    bytes.insert(bytes.end(), payload.Bytes().begin(), payload.Bytes().end());
    return bytes;
}

Emulator::PreparedStateRestore Emulator::PrepareStateRestore(std::span<const uint8_t> bytes,
                                                             const StateOperation& operation) {
    if (!initialized_)
        throw std::runtime_error("Machine state requires a loaded cartridge");
    if (bytes.size() < kHeaderBytes || bytes.size() > StateArchive::MaximumBytes)
        throw std::runtime_error("Invalid machine state size");
    operation.Report(StatePhase::Validate);
    auto prepared = std::make_unique<PreparedStateRestore::Impl>();
    prepared->bytes.assign(bytes.begin(), bytes.end());
    const std::span<const uint8_t> owned(prepared->bytes);
    const auto payload = owned.subspan(kHeaderBytes);
    StateArchive header(owned.first(kHeaderBytes));
    for (auto byte : kMagic)
        header.Expect(byte);
    header.Expect(kVersion);
    header.Expect(uint32_t(payload.size()));
    for (auto byte : detail::StateDigest(payload, operation, StatePhase::Validate))
        header.Expect(byte);
    header.Commit();
    prepared->archive = std::make_unique<StateArchive>(payload);
    prepared->archive->SetProgress(operation);
    SerializeState(*prepared->archive);
    if (prepared->archive->Remaining())
        throw std::runtime_error("Unexpected data after machine state");
    if (!restoreLifetime_)
        restoreLifetime_ = std::make_shared<uint8_t>(0);
    prepared->lifetime = restoreLifetime_;
    return PreparedStateRestore(std::move(prepared));
}

void Emulator::RestoreState(std::span<const uint8_t> bytes, const StateOperation& operation) {
    auto prepared = PrepareStateRestore(bytes, operation);
    prepared.Commit();
}

void Emulator::SerializeState(StateArchive& archive) {
    archive.Validate(initialized_ && cartridge_ && cpu_, "Machine state requires loaded hardware");
    archive.Validate(archive.Reading() ||
                         (atStateBoundary_ && !collectingAudio_ && !synchronizingState_),
                     "Machine state requires synchronized hardware");
    archive.Section(0x4d414348, [&](auto& fields) {
        fields.Expect(uint32_t(config_.expansionDevice));
        fields.Expect(bool(dsp1_));
        fields.Expect(bool(dsp2_));
        fields.Expect(bool(srtc_));
        fields.Expect(bool(msu1_));
        fields.Section(1, [&](auto& part) { cartridge_->SerializeState(part); });
        fields.Section(2, [&](auto& part) { cpu_->SerializeState(part); });
        fields.Section(3, [&](auto& part) { bus_.SerializeState(part); });
        fields.Section(4, [&](auto& part) { ppu_.SerializeState(part); });
        fields.Section(5, [&](auto& part) { smp_.SerializeState(part); });
        fields.Section(6, [&](auto& part) { dsp_.SerializeState(part); });
        fields.Section(7, [&](auto& part) { timing_.SerializeState(part); });
        fields.Section(8, [&](auto& part) { cpuIo_.SerializeState(part); });
        fields.Section(9, [&](auto& part) { dma_.SerializeState(part); });
        fields.Section(10, [&](auto& part) { irq_.SerializeState(part); });
        fields.Section(11, [&](auto& part) { autoJoypad_.SerializeState(part); });
        fields.Section(12, [&](auto& part) { broadcastReceiver_.SerializeState(part); });
        fields.Section(13, [&](auto& part) { audioMixer_.SerializeState(part); });
        if (dsp1_)
            fields.Section(14, [&](auto& part) { dsp1_->SerializeState(part); });
        if (dsp2_)
            fields.Section(15, [&](auto& part) { dsp2_->SerializeState(part); });
        if (srtc_)
            fields.Section(16, [&](auto& part) { srtc_->SerializeState(part); });
        if (msu1_)
            fields.Section(17, [&](auto& part) { msu1_->SerializeState(part); });
        fields.Expect(bool(startupEntropy_));
        if (startupEntropy_)
            startupEntropy_->SerializeState(fields);
        if (cartridge_->Header().chip != EnhancementChip::SuperGameBoy)
            handheldStartup_->SerializeState(fields);
        fields.Object(cx4Storage_, [](auto& storageFields, auto& next) {
            bool present = next.has_value();
            storageFields.Number(present);
            if (!present) {
                next.reset();
                return;
            }
            if (!next)
                next.emplace();
            for (auto& page : next->program)
                storageFields.Array(page);
            for (auto& word : next->stack)
                storageFields.Bounded(word, uint32_t{0}, uint32_t{0xffffff});
        });
        fields.Number(frameIndex_);
        fields.Number(masterCycles_);
        fields.Number(coprocessorsSyncedClock_);
        fields.Number(coprocessorClockTarget_);
        fields.Number(coprocessorCompletedClock_);
        fields.Number(msuSamplesClocked_);
        fields.Bounded(pendingExtraClocks_, 0u, 1u << 20);
        fields.Number(pendingHdmaSetup_);
        fields.Number(pendingHdmaRun_);
        const auto hdma = fields.Number(servicingHdma_);
        fields.Validate(!hdma, "Machine state retains unfinished HDMA execution");
        fields.Number(dmaArmed_);
        const auto open = fields.Number(frameOpen_);
        fields.Object(frameInput_,
                      [](auto& inputFields, auto& input) { Input(inputFields, input); });
        fields.Expect(audioOutputBuffer_.sampleRate);
        const auto audio =
            fields.Vector(audioOutputBuffer_.interleavedStereo, 4 * kAudioBufSamples);
        fields.Validate(audio % 2 == 0 && (open || audio == 0), "Invalid pending frame audio");
        const auto width = fields.Bounded(videoOutputFrame_.width, 1u, uint32_t(Ppu::OutputWidth));
        const auto height =
            fields.Bounded(videoOutputFrame_.height, 1u, uint32_t(Ppu::OutputHeight));
        const auto pixels =
            fields.Vector(videoOutputFrame_.pixels, Ppu::OutputWidth * Ppu::OutputHeight);
        fields.Validate(!pixels || pixels == size_t(width) * height,
                        "Invalid completed frame size");
        fields.BeforeRestore([this]() noexcept {
            atStateBoundary_ = true;
            collectingAudio_ = synchronizingState_ = singleInstruction_ = false;
            yieldReason_ = YieldReason::None;
        });
    });
}
} // namespace snes::core
