#include "snes/core/Dsp3.hpp"
#include "snes/core/Dsp4.hpp"
#include "snes/core/detail/StateArchive.hpp"

namespace snes::core {
void Dsp3::SerializeState(detail::StateArchive &archive) {
  archive.Object(*this, [](auto &fields, auto &dsp) {
    fields.Enumeration(dsp.phase_, Phase::Command, Phase::ResultCost);
    fields.Number(dsp.data_);
    fields.Number(dsp.status_);
    fields.Bounded(dsp.index_, uint16_t{0}, uint16_t{1024});
    fields.Number(dsp.count_);
    fields.Number(dsp.width_);
    fields.Number(dsp.height_);
    fields.Number(dsp.moveX_);
    fields.Number(dsp.moveY_);
    fields.Number(dsp.x_);
    fields.Number(dsp.y_);
    fields.Array(dsp.pixels_);
    fields.Array(dsp.planes_);
    fields.Enumeration(dsp.decodePhase_, DecodePhase::SymbolPrefix, DecodePhase::CopyOffset);
    fields.Array(dsp.symbols_);
    fields.Array(dsp.offsets_);
    for (auto &length : dsp.lengths_)
      fields.Bounded(length, uint8_t{0}, uint8_t{8});
    fields.Number(dsp.symbolsLeft_);
    fields.Number(dsp.outputsLeft_);
    fields.Number(dsp.symbol_);
    fields.Number(dsp.bitWord_);
    fields.Number(dsp.partialBits_);
    fields.Bounded(dsp.bitsAvailable_, 0u, 16u);
    fields.Bounded(dsp.bitsNeeded_, 0u, 12u);
    fields.Bounded(dsp.symbolPrefix_, 0u, 3u);
    fields.Bounded(dsp.treeEntries_, 0u, 8u);
    fields.Number(dsp.prefixBits_);
    fields.Validate(dsp.prefixBits_ == 0 || dsp.prefixBits_ == 2 || dsp.prefixBits_ == 3,
                    "Invalid DSP-3 prefix width");
    fields.Bounded(dsp.prefix_, 0u, 7u);
    fields.Number(dsp.copyBits_);
    fields.Validate(dsp.copyBits_ == 0 || dsp.copyBits_ == 8 || dsp.copyBits_ == 12,
                    "Invalid DSP-3 copy width");
    fields.Array(dsp.cells_, [](auto &cellFields, auto &cell) {
      cellFields.Number(cell.terrain);
      cellFields.Number(cell.cost);
      cellFields.Number(cell.weight);
    });
    auto &ring = dsp.ring_;
    fields.Number(ring.x);
    fields.Number(ring.y);
    fields.Bounded(ring.low, 0u, 256u);
    fields.Bounded(ring.high, 0u, 255u);
    fields.Bounded(ring.radius, 0u, 256u);
    fields.Bounded(ring.steps, 0u, ring.radius);
    fields.Bounded(ring.side, 0u, 6u);
    fields.Number(dsp.originX_);
    fields.Number(dsp.originY_);
    fields.Bounded(dsp.searchedRadius_, 0u, 255u);
    fields.Bounded(dsp.returnedRadius_, 0u, 255u);
    fields.Number(dsp.cell_);
    if (dsp.phase_ == Phase::PlanarData || dsp.phase_ == Phase::PlanarRead)
      fields.Validate((dsp.index_ & 1) == 0 &&
                          dsp.index_ <= (dsp.phase_ == Phase::PlanarData ? 6 : 8),
                      "Invalid DSP-3 planar position");
    if (dsp.phase_ == Phase::Decode) {
      fields.Validate(dsp.symbolsLeft_ <= 512 && dsp.outputsLeft_ != 0,
                      "Invalid DSP-3 active decode counts");
      if (dsp.decodePhase_ == DecodePhase::SymbolPrefix ||
          dsp.decodePhase_ == DecodePhase::SymbolValue)
        fields.Validate(dsp.index_ < 512 && dsp.symbolsLeft_ != 0, "Invalid DSP-3 symbol position");
      if (dsp.decodePhase_ == DecodePhase::TreeLength)
        fields.Validate(dsp.index_ < 8 && dsp.treeEntries_ != 0, "Invalid DSP-3 tree position");
      if (dsp.decodePhase_ >= DecodePhase::Prefix)
        fields.Validate(dsp.prefixBits_ != 0, "Invalid DSP-3 active prefix width");
    }
    if (dsp.phase_ == Phase::SearchCell || dsp.phase_ == Phase::Terrain ||
        dsp.phase_ == Phase::Cost || dsp.phase_ == Phase::ResultCell)
      fields.Validate(ring.steps != 0 && ring.side < 6, "Invalid DSP-3 active ring position");
  });
}

void Dsp4::SerializeState(detail::StateArchive &archive) {
  archive.Array(input_);
  const auto output = archive.Vector(output_, 1024 * 1024);
  const auto received = archive.Bounded(received_, 0u, 96u);
  const auto needed = archive.Bounded(needed_, 0u, 96u);
  archive.Bounded(cursor_, 0u, 96u);
  archive.Bounded(outputCursor_, 0u, uint32_t(output));
  archive.Bounded(commandBytes_, 0u, 1u);
  archive.Number(command_);
  const auto ready = archive.Number(commandReady_);
  archive.Validate(ready || received < needed, "Invalid DSP-4 input position");
  archive.Enumeration(stage_, Stage::Start, Stage::Tile);
  archive.Number(distance_);
  archive.Number(raster_);
  archive.Array(polygons_, [](auto &fields, auto &polygon) {
    fields.Array(polygon.edges, [](auto &edgeFields, auto &edge) {
      edgeFields.Number(edge.left);
      edgeFields.Number(edge.right);
      edgeFields.Number(edge.center);
      edgeFields.Number(edge.pointer);
      edgeFields.Number(edge.top);
      edgeFields.Number(edge.bottom);
      edgeFields.Number(edge.raster);
    });
    fields.Number(polygon.start);
    fields.Number(polygon.plane);
  });
  archive.Object(road_, [](auto &fields, auto &road) {
    fields.Number(road.x);
    fields.Number(road.y);
    fields.Number(road.dx);
    fields.Number(road.dy);
    fields.Number(road.envelope);
    fields.Number(road.ddx);
    fields.Number(road.ddy);
    fields.Number(road.vertical);
    fields.Number(road.verticalEnvelope);
    const auto point = [](auto &pointFields, auto &value) {
      pointFields.Number(value.x);
      pointFields.Number(value.y);
    };
    fields.Object(road.previous, point);
    fields.Object(road.next, point);
    fields.Object(road.previousScroll, point);
    fields.Object(road.nextScroll, point);
    fields.Object(road.delta, point);
    fields.Number(road.turn);
    fields.Number(road.turnDelta);
    fields.Number(road.lines);
    fields.Bounded(road.light, 0u, 4u);
  });
  archive.Object(viewport_, [](auto &fields, auto &viewport) {
    fields.Number(viewport.x);
    fields.Number(viewport.y);
    fields.Number(viewport.left);
    fields.Number(viewport.right);
    fields.Number(viewport.top);
    fields.Number(viewport.bottom);
  });
  archive.Object(object_, [](auto &fields, auto &object) {
    fields.Number(object.x);
    fields.Number(object.y);
    fields.Number(object.attributes);
    fields.Number(object.clip);
    fields.Number(object.header);
    fields.Number(object.large);
  });
  archive.Array(attributes_);
  archive.Array(rows_);
  archive.Bounded(objectCount_, 0u, 128u);
  archive.Number(rowLimit_);
}
} // namespace snes::core
