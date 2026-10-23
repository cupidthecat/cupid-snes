#include "snes/core/Emulator.hpp"
#include "snes/core/SnesCpu.hpp"
#include "snes/core/detail/ExecutionContext.hpp"

#include <algorithm>
#include <stdexcept>

namespace snes::core {
void Emulator::BeginFrame() {
  if (frameOpen_)
    return;
  autoJoypad_.Ports().PollLightGuns();
  frameInput_ = inputProvider_ ? inputProvider_->PollController(0, frameIndex_) : InputState{};
  // Prepare DSP audio buffer for this frame
  audioOutputBuffer_.interleavedStereo.clear();
  dsp_.ResetSamplesWritten();
  dsp_.SetOutput(audioBuf_.data(), kAudioBufSamples);
  frameOpen_ = true;
}

void Emulator::ResumeFrameExecution(bool instructionBoundary) {
  if (!execution_) {
    execution_ = std::make_unique<detail::ExecutionContext>([this]() {
      try {
        for (;;) {
          cpu_->Step();
          if (singleInstruction_) {
            atStateBoundary_ = true;
            yieldReason_ = YieldReason::Instruction;
            execution_->Suspend();
            atStateBoundary_ = false;
          }
        }
      } catch (...) {
        cpu_->stepSuspended_ = false;
        throw;
      }
    });
  }
  singleInstruction_ = instructionBoundary;
  atStateBoundary_ = false;
  yieldReason_ = YieldReason::None;
  collectingAudio_ = true;
  try {
    execution_->Resume();
  } catch (...) {
    collectingAudio_ = singleInstruction_ = false;
    throw;
  }
  collectingAudio_ = singleInstruction_ = false;
}

FrameStepResult Emulator::FinishFrame() {
  // End-of-frame processing
  // RenderFrame is now called from VBlankBegin() - no separate call here.
  // The previous duplicate call consumed V=1 from the next frame, causing
  // alternating 1-line / 223-line renders and incorrect scroll values.

  // Submit the aligned stereo stream.
  if (audioOutput_) {
    audioOutputBuffer_.sampleRate = AudioTiming::SampleRate;
    audioOutput_->Submit(audioOutputBuffer_);
  }

  // Present video - extract only the visible region from the PPU's
  // internal buffer. Visible rows are written starting at output row 0,
  // so the frontend can copy them directly without preserving the hidden
  // 240-line border area.
  auto &frame = videoOutputFrame_;
  frame.width = ppu_.FrameWidth();
  frame.height = ppu_.FrameHeight();
  const auto count = static_cast<size_t>(frame.width) * frame.height;
  if (const uint32_t *src = ppu_.OutputData()) {
    frame.pixels.resize(count);
    for (uint32_t y = 0; y < frame.height; ++y)
      std::copy_n(src + static_cast<size_t>(y) * Ppu::OutputWidth, frame.width,
                  frame.pixels.data() + static_cast<size_t>(y) * frame.width);
  } else {
    frame.pixels.assign(count, 0xFF000000);
  }
  if (videoOutput_)
    videoOutput_->Present(frame);

  if (!cheats_.Empty())
    cheats_.Refresh([this](uint32_t address, uint8_t value) { bus_.WriteCheat(address, value); });

  // Update counters
  masterCycles_ = timing_.MasterClocksElapsed();
  ++frameIndex_;
  frameOpen_ = false;
  audioOutputBuffer_.interleavedStereo.clear();
  if (inputProvider_)
    inputProvider_->AdvanceFrame();
  return {.frameIndex = frameIndex_, .masterCycles = masterCycles_, .latchedInput = frameInput_};
}

void Emulator::SynchronizeState(const StateOperation &operation) {
  if (!initialized_)
    throw std::runtime_error("Machine state requires a loaded cartridge");
  synchronizingState_ = true;
  try {
    for (;;) {
      operation.Report(StatePhase::Synchronize);
      if (atStateBoundary_) {
        const auto first = dsp_.SamplesWritten();
        collectingAudio_ = frameOpen_;
        const auto ready =
            smp_.TrySynchronizeState(coprocessorCompletedClock_, timing_.MasterClockHz());
        RouteDspSamples(first);
        collectingAudio_ = false;
        if (ready)
          break;
      }
      BeginFrame();
      ResumeFrameExecution(true);
      if (yieldReason_ == YieldReason::Frame)
        FinishFrame();
    }
    cartridge_->SynchronizeState();
  } catch (...) {
    synchronizingState_ = collectingAudio_ = singleInstruction_ = false;
    throw;
  }
  synchronizingState_ = false;
}
} // namespace snes::core
