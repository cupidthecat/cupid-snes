#include "snes/core/Dsp1.hpp"
#include "snes/core/Dsp2.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Dsp1::SerializeState(detail::StateArchive &archive) {
  archive.Object(*this, [](auto &fields, auto &dsp) {
    const auto matrix = [](auto &values, auto &matrix) {
      for (auto &row : matrix)
        values.Fixed(std::span<int16_t>(row));
    };
    matrix(fields, dsp.shared_.MatrixA);
    matrix(fields, dsp.shared_.MatrixB);
    matrix(fields, dsp.shared_.MatrixC);
    auto &shared = dsp.shared_;
    fields.Number(shared.CentreX);
    fields.Number(shared.CentreY);
    fields.Number(shared.CentreZ);
    fields.Number(shared.CentreZ_C);
    fields.Number(shared.CentreZ_E);
    fields.Number(shared.VOffset);
    fields.Number(shared.Les);
    fields.Number(shared.C_Les);
    fields.Number(shared.E_Les);
    fields.Number(shared.SinAas);
    fields.Number(shared.CosAas);
    fields.Number(shared.SinAzs);
    fields.Number(shared.CosAzs);
    fields.Number(shared.SinAZS);
    fields.Number(shared.CosAZS);
    fields.Number(shared.SecAZS_C1);
    fields.Number(shared.SecAZS_E1);
    fields.Number(shared.SecAZS_C2);
    fields.Number(shared.SecAZS_E2);
    fields.Number(shared.Nx);
    fields.Number(shared.Ny);
    fields.Number(shared.Nz);
    fields.Number(shared.Gx);
    fields.Number(shared.Gy);
    fields.Number(shared.Gz);
    fields.Number(shared.Hx);
    fields.Number(shared.Hy);
    fields.Number(shared.Vx);
    fields.Number(shared.Vy);
    fields.Number(shared.Vz);
    fields.Number(dsp.sr_);
    fields.Number(dsp.statusReadHigh_);
    fields.Number(dsp.dr_);
    fields.Bounded(dsp.fsmMajorState_, unsigned(WAIT_COMMAND), unsigned(WRITE_DATA));
    fields.Number(dsp.command_);
    fields.Number(dsp.dataCounter_);
    fields.Fixed(std::span<int16_t>(dsp.readBuffer_));
    fields.Fixed(std::span<int16_t>(dsp.writeBuffer_));
    fields.Number(dsp.freeze_);
    if (dsp.fsmMajorState_ != WAIT_COMMAND) {
      fields.Validate(dsp.command_ < 64 && kCommandTable[dsp.command_].callback,
                      "Invalid DSP-1 active command");
      const auto count = dsp.fsmMajorState_ == READ_DATA ? kCommandTable[dsp.command_].reads
                                                         : kCommandTable[dsp.command_].writes;
      fields.Validate(dsp.dataCounter_ < count &&
                          (dsp.fsmMajorState_ != READ_DATA || dsp.dataCounter_ < MAX_READS),
                      "Invalid DSP-1 transfer position");
    }
  });
}

void Dsp2::SerializeState(detail::StateArchive &archive) {
  archive.Object(*this, [](auto &fields, auto &dsp) {
    fields.Array(dsp.input_);
    fields.Array(dsp.output_);
    fields.Bounded(dsp.received_, 0u, 511u);
    fields.Bounded(dsp.needed_, 0u, 510u);
    fields.Bounded(dsp.read_, 0u, 511u);
    fields.Bounded(dsp.available_, 0u, 255u);
    for (auto &length : dsp.lengths_)
      fields.Bounded(length, 0u, 255u);
    fields.Array(dsp.payloads_);
    fields.Bounded(dsp.scaledLength_, 0u, 255u);
    fields.Number(dsp.command_);
    fields.Number(dsp.transparent_);
    fields.Number(dsp.commandReady_);
  });
}
} // namespace snes::core
