#include "FramePacer.hpp"
#include "snes/core/Timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

using snes::core::Region;
using snes::core::Timing;
using snes::frontend::FramePacer;

namespace {
void Check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void ConsoleClockDeadlines() {
  for (Region region : {Region::NTSC, Region::PAL}) {
    for (bool interlace : {false, true}) {
      Timing timing(region);
      timing.SetInterlace(interlace);
      FramePacer pacer;
      const uint64_t epoch = 5'000'000'000;
      pacer.Schedule(epoch, 0, timing.MasterClockHz(), FramePacer::kTargetAudioQueueMilliseconds);
      uint64_t clocks = 0, deadline = epoch;
      for (unsigned frame = 0; frame < 600; ++frame) {
        const auto before = timing.MasterClocksElapsed();
        const auto target = timing.FrameCount() + 1;
        while (timing.FrameCount() < target)
          timing.Tick(2);
        const auto elapsed = timing.MasterClocksElapsed() - before;
        clocks += elapsed;
        deadline = pacer.Schedule(deadline + 3'000'000, elapsed, timing.MasterClockHz(),
                                  FramePacer::kTargetAudioQueueMilliseconds);
        const double expected = double(clocks) * 1'000'000'000 / timing.MasterClockHz();
        Check(std::abs(double(deadline - epoch) - expected) < 1.1,
              "Presentation preserves region, field lengths and fractional clock time");
      }
    }
  }
}

void StallsAndMultipleFields() {
  FramePacer pacer;
  const uint64_t epoch = 1'000'000'000;
  const auto clocks = Timing::kMasterClocksPerFrameNTSC;
  pacer.Schedule(epoch, clocks, Timing::kMasterClockHz, FramePacer::kTargetAudioQueueMilliseconds);
  auto due = pacer.Schedule(epoch + 100'000'000, clocks, Timing::kMasterClockHz,
                            FramePacer::kTargetAudioQueueMilliseconds);
  Check(due == epoch + 100'000'000, "A stalled frame can be shown immediately");
  const auto next = pacer.Schedule(due + 2'000'000, clocks, Timing::kMasterClockHz,
                                   FramePacer::kTargetAudioQueueMilliseconds);
  Check(next - due > 16'000'000, "Recovery has no burst of overdue frames");
  due = pacer.Schedule(next + 2'000'000, uint64_t(clocks) * 4, Timing::kMasterClockHz,
                       FramePacer::kTargetAudioQueueMilliseconds);
  Check(due - next > 66'000'000 && due - next < 67'000'000,
        "A transfer spanning multiple fields retains all elapsed console time");
  pacer.Reset();
  Check(pacer.Schedule(due + 500'000'000, clocks, Timing::kMasterClockHz,
                       FramePacer::kTargetAudioQueueMilliseconds) == due + 500'000'000,
        "Refilling an audio stream starts a fresh presentation epoch");
}

void DelayedPresentation() {
  FramePacer pacer;
  const auto clocks = Timing::kMasterClocksPerFrameNTSC;
  const double period = double(clocks) * 1'000'000'000 / Timing::kMasterClockHz;
  uint64_t due = 0;
  for (uint64_t delay : {8'000'000, 20'000'000, 100'000'000}) {
    pacer.Reset();
    due = pacer.Schedule(1'000'000'000, clocks, Timing::kMasterClockHz,
                         FramePacer::kTargetAudioQueueMilliseconds);
    pacer.Presented(due);
    due = pacer.Schedule(due + 2'000'000, clocks, Timing::kMasterClockHz,
                         FramePacer::kTargetAudioQueueMilliseconds);
    const auto late = due + delay;
    pacer.Presented(late);
    const auto next = pacer.Schedule(late + 2'000'000, clocks, Timing::kMasterClockHz,
                                     FramePacer::kTargetAudioQueueMilliseconds);
    if (double(delay) < period)
      Check(std::abs(double(next - due) - period) < 1.1,
            "A renderer delay within one picture retains the console clock");
    else
      Check(std::abs(double(next - late) - period) < 1.1,
            "A full-picture stall cannot bunch subsequent overdue pictures");
  }

  // Timer wake-up noise must not accumulate into a slower emulation clock.
  pacer.Reset();
  const uint64_t epoch = 2'000'000'000;
  due = pacer.Schedule(epoch, clocks, Timing::kMasterClockHz,
                       FramePacer::kTargetAudioQueueMilliseconds);
  for (unsigned i = 0; i < 600; ++i) {
    due = pacer.Schedule(due + 500'000 + 2'000'000, clocks, Timing::kMasterClockHz,
                         FramePacer::kTargetAudioQueueMilliseconds);
    pacer.Presented(due + 500'000);
  }
  const double expected = double(clocks) * 600 * 1'000'000'000 / Timing::kMasterClockHz;
  Check(std::abs(double(due - epoch) - expected) < 1.1,
        "Small presentation jitter does not accumulate as clock drift");
}

void DelayedComputation() {
  const auto clocks = Timing::kMasterClocksPerFrameNTSC;
  const double period = double(clocks) * 1'000'000'000 / Timing::kMasterClockHz;
  const uint64_t epoch = 1'000'000'000;
  for (uint64_t delay : {8'000'000, 20'000'000, 100'000'000}) {
    FramePacer pacer;
    pacer.Schedule(epoch, clocks, Timing::kMasterClockHz,
                   FramePacer::kTargetAudioQueueMilliseconds);
    const auto ready = epoch + uint64_t(period) + delay;
    const auto due = pacer.Schedule(ready, clocks, Timing::kMasterClockHz,
                                    FramePacer::kTargetAudioQueueMilliseconds);
    if (double(delay) < period)
      Check(std::abs(double(due - epoch) - period) < 1.1,
            "Computation slightly past its deadline retains the console clock");
    else
      Check(due == ready, "A full-picture computation stall starts a fresh cadence");
    pacer.Presented(ready);
    const auto next = pacer.Schedule(ready + 2'000'000, clocks, Timing::kMasterClockHz,
                                     FramePacer::kTargetAudioQueueMilliseconds);
    const double elapsed = double(next - (double(delay) < period ? epoch : ready));
    Check(std::abs(elapsed - period * (double(delay) < period ? 2 : 1)) < 1.1,
          "Computation recovery retains short delays and clears long stalls");
  }
}

// Simulate audio callbacks that drain a 21.3 ms block at once, with a device
// clock slightly faster or slower than the console. This used to release
// multiple frames at one callback followed by a long wait at the next one.
void BatchedAudioDemand() {
  const double frameNs =
      double(Timing::kMasterClocksPerFrameNTSC) * 1'000'000'000 / Timing::kMasterClockHz;
  for (double deviceSpeed : {0.996, 1.0, 1.004}) {
    FramePacer pacer;
    double queuedMs = 50;
    uint64_t shown = 1'000'000'000;
    double nextCallback = double(shown) + 21'333'333 / deviceSpeed;
    const double callbackPeriod = 21'333'333 / deviceSpeed;
    pacer.Schedule(shown, 0, Timing::kMasterClockHz, FramePacer::kTargetAudioQueueMilliseconds);
    auto drain = [&](uint64_t until) {
      while (nextCallback <= double(until)) {
        queuedMs -= 21.333333;
        Check(queuedMs > 0, "Batched device consumption must not underrun during steady playback");
        nextCallback += callbackPeriod;
      }
    };
    for (unsigned frame = 0; frame < 12'000; ++frame) {
      const auto ready = shown + (frame % 3 + 1) * 2'000'000;
      drain(ready);
      queuedMs += frameNs / 1'000'000;
      const auto due = pacer.Schedule(ready, Timing::kMasterClocksPerFrameNTSC,
                                      Timing::kMasterClockHz, queuedMs);
      Check(double(due - shown) >= frameNs * 0.9949 && double(due - shown) <= frameNs * 1.0051,
            "Audio callbacks and variable computation cannot bunch normal presentations");
      drain(due);
      Check(queuedMs < 80, "Clock drift must not grow an unbounded audio backlog");
      pacer.Presented(due);
      shown = due;
    }
  }
}

void RendererDelaysKeepAudioFed() {
  const auto clocks = Timing::kMasterClocksPerFrameNTSC;
  const double frameNs = double(clocks) * 1'000'000'000 / Timing::kMasterClockHz;
  for (double deviceSpeed : {0.996, 1.0, 1.004}) {
    FramePacer pacer;
    double queuedMs = 50;
    uint64_t shown = 1'000'000'000;
    double nextCallback = double(shown) + 10'000'000 / deviceSpeed;
    pacer.Schedule(shown, 0, Timing::kMasterClockHz, FramePacer::kTargetAudioQueueMilliseconds);
    const auto drain = [&](uint64_t until) {
      while (nextCallback <= double(until)) {
        queuedMs -= 10;
        Check(queuedMs > 0, "Short renderer delays must not starve steady audio playback");
        nextCallback += 10'000'000 / deviceSpeed;
      }
    };
    for (unsigned frame = 0; frame < 12'000; ++frame) {
      const auto ready = shown + (6 + frame % 3) * 1'000'000;
      drain(ready);
      queuedMs += frameNs / 1'000'000;
      const auto due = pacer.Schedule(ready, clocks, Timing::kMasterClockHz, queuedMs);
      drain(due);
      shown = due + (frame % 20 == 0 ? 8'000'000 : 0);
      drain(shown);
      pacer.Presented(shown);
      Check(queuedMs < 80, "Renderer recovery keeps the audio backlog bounded");
    }
  }
}

void VariableComputationKeepsAudioFed() {
  const auto clocks = Timing::kMasterClocksPerFrameNTSC;
  const double frameNs = double(clocks) * 1'000'000'000 / Timing::kMasterClockHz;
  for (double deviceSpeed : {0.996, 1.0, 1.004}) {
    FramePacer pacer;
    double queuedMs = 50;
    uint64_t shown = 1'000'000'000;
    double nextCallback = double(shown) + 10'000'000 / deviceSpeed;
    pacer.Schedule(shown, 0, Timing::kMasterClockHz, FramePacer::kTargetAudioQueueMilliseconds);
    const auto drain = [&](uint64_t until) {
      while (nextCallback <= double(until)) {
        queuedMs -= 10;
        Check(queuedMs > 0, "Variable computation within the average budget keeps audio fed");
        nextCallback += 10'000'000 / deviceSpeed;
      }
    };
    for (unsigned frame = 0; frame < 12'000; ++frame) {
      const auto ready = shown + (15 + frame % 3) * 1'000'000;
      drain(ready);
      queuedMs += frameNs / 1'000'000;
      const auto due = pacer.Schedule(ready, clocks, Timing::kMasterClockHz, queuedMs);
      shown = std::max(ready, due) + 350'000;
      drain(shown);
      pacer.Presented(shown);
      Check(queuedMs < 80, "Variable computation keeps the audio backlog bounded");
    }
  }
}

void PreparationSpikesKeepAudioFed() {
  for (Region region : {Region::NTSC, Region::PAL}) {
    Timing timing(region);
    const auto clocks = region == Region::PAL ? Timing::kMasterClocksPerFramePAL
                                              : Timing::kMasterClocksPerFrameNTSC;
    const double frameNs = double(clocks) * 1'000'000'000 / timing.MasterClockHz();
    for (double deviceSpeed : {0.996, 1.0, 1.004}) {
      for (unsigned phase = 0; phase < 20; ++phase) {
        FramePacer pacer;
        double queuedMs = FramePacer::kTargetAudioQueueMilliseconds;
        uint64_t shown = 1'000'000'000;
        double nextCallback = double(shown) + phase * 500'000 / deviceSpeed;
        pacer.Schedule(shown, 0, timing.MasterClockHz(), queuedMs);
        const auto drain = [&](uint64_t until) {
          while (nextCallback <= double(until)) {
            queuedMs -= 10;
            Check(queuedMs > 0, "A short preparation spike cannot drain the device callback queue");
            nextCallback += 10'000'000 / deviceSpeed;
          }
        };
        for (unsigned frame = 0; frame < 1800; ++frame) {
          const auto ready = shown + (frame % 180 == 179 ? 24'000'000 : 6'000'000);
          drain(ready);
          queuedMs += frameNs / 1'000'000;
          const auto due = pacer.Schedule(ready, clocks, timing.MasterClockHz(), queuedMs);
          shown = std::max(ready, due) + (frame % 91 == 90 ? 2'500'000 : 0);
          drain(shown);
          pacer.Presented(shown);
          Check(queuedMs < 100, "Spike recovery keeps the audio backlog bounded");
        }
      }
    }
  }
}
} // namespace

int main() {
  try {
    ConsoleClockDeadlines();
    StallsAndMultipleFields();
    DelayedPresentation();
    DelayedComputation();
    BatchedAudioDemand();
    RendererDelaysKeepAudioFed();
    VariableComputationKeepsAudioFed();
    PreparationSpikesKeepAudioFed();
    std::puts("Console clock, audio callback, drift and stall pacing checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
