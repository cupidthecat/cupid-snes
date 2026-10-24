#include "snes/core/Spc7110Decoder.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Spc7110Decoder::SerializeState(detail::StateArchive& archive) {
    archive.Array(contexts_, [](auto& fields, auto& context) {
        fields.Bounded(context.model, uint8_t{0}, uint8_t{52});
        fields.Number(context.inverted);
    });
    uint16_t seen = 0;
    bool cold = true;
    for (auto& color : colors_) {
        const auto next = archive.Bounded(color, uint8_t{0}, uint8_t{15});
        seen |= uint16_t(1u << next);
        cold = cold && next == 0;
    }
    archive.Number(address_);
    archive.Bounded(interval_, uint16_t{128}, uint16_t{256});
    archive.Number(code_);
    archive.Number(history_);
    archive.Number(decisions_);
    archive.Bounded(remaining_, uint8_t{1}, uint8_t{8});
    const auto depth = archive.Number(depth_);
    archive.Validate(depth == 1 || depth == 2 || depth == 4, "Invalid compression bit depth");
    archive.Validate((depth == 1 && cold) || seen == 0xffff,
                     "Invalid compression color permutation");
}
} // namespace snes::core
