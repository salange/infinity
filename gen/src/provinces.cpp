#include "gen/provinces.hpp"

#include <array>
#include <cstdio>
#include <set>

namespace inf::gen {

using det::Real;

namespace {

Real u01(std::uint64_t word) {
  return Real(static_cast<double>(word >> 11U) * 0x1.0p-53);
}

Real uniform(std::uint64_t word, double lo, double hi) {
  return Real(lo) + Real(hi - lo) * u01(word);
}

struct ArchetypeSpec {
  Archetype archetype;
  std::uint32_t weight;
  double relief_lo, relief_hi;  // m
  double base_lo, base_hi;      // m
  double rug_lo, rug_hi;        // [0,1]
  double carve_lo, carve_hi;    // [0,1]
};

// Per-type archetype tables (prototype-v0 spec section 5): data, not code.
constexpr std::array<ArchetypeSpec, 5> kEarthLike = {{
    {Archetype::Flats, 3, 30, 80, -50, 50, 0.10, 0.30, 0.00, 0.10},
    {Archetype::RollingHills, 3, 120, 300, 0, 150, 0.30, 0.50, 0.10, 0.20},
    {Archetype::Alpine, 2, 900, 2200, 300, 800, 0.70, 1.00, 0.30, 0.60},
    {Archetype::Canyon, 2, 250, 600, 100, 400, 0.50, 0.70, 0.70, 1.00},
    {Archetype::HighlandPlateau, 2, 150, 350, 400, 900, 0.20, 0.40, 0.20, 0.40},
}};
constexpr std::array<ArchetypeSpec, 3> kBarren = {{
    {Archetype::RegolithPlains, 3, 40, 120, -30, 60, 0.20, 0.40, 0.00, 0.10},
    {Archetype::Cratered, 3, 150, 400, 0, 100, 0.50, 0.80, 0.40, 0.70},
    {Archetype::Highlands, 2, 300, 800, 200, 600, 0.50, 0.80, 0.10, 0.30},
}};
constexpr std::array<ArchetypeSpec, 3> kDesert = {{
    {Archetype::Dunes, 3, 80, 200, -20, 80, 0.30, 0.50, 0.00, 0.10},
    {Archetype::Mesas, 2, 250, 500, 100, 300, 0.40, 0.60, 0.60, 0.90},
    {Archetype::Canyonlands, 2, 300, 700, 50, 250, 0.60, 0.80, 0.70, 1.00},
}};
constexpr std::array<ArchetypeSpec, 3> kIce = {{
    {Archetype::GlacialShield, 3, 50, 150, 0, 200, 0.10, 0.30, 0.00, 0.10},
    {Archetype::CrevasseField, 2, 200, 450, 50, 250, 0.60, 0.90, 0.50, 0.80},
    {Archetype::RidgeField, 2, 500, 1200, 200, 600, 0.70, 1.00, 0.30, 0.50},
}};

struct TableView {
  const ArchetypeSpec* specs;
  std::size_t count;
};

TableView table_for(PlanetType type) {
  switch (type) {
    case PlanetType::EarthLike: return {kEarthLike.data(), kEarthLike.size()};
    case PlanetType::Barren: return {kBarren.data(), kBarren.size()};
    case PlanetType::Desert: return {kDesert.data(), kDesert.size()};
    case PlanetType::Ice: return {kIce.data(), kIce.size()};
  }
  return {kEarthLike.data(), kEarthLike.size()};
}

const ArchetypeSpec& weighted_pick(const TableView& table, std::uint64_t word) {
  std::uint32_t total = 0;
  for (std::size_t i = 0; i < table.count; ++i) {
    total += table.specs[i].weight;
  }
  std::uint32_t roll = static_cast<std::uint32_t>((word >> 32U) % total);
  for (std::size_t i = 0; i < table.count; ++i) {
    if (roll < table.specs[i].weight) {
      return table.specs[i];
    }
    roll -= table.specs[i].weight;
  }
  return table.specs[table.count - 1];
}

}  // namespace

const char* to_string(Archetype archetype) {
  switch (archetype) {
    case Archetype::Flats: return "Flats";
    case Archetype::RollingHills: return "RollingHills";
    case Archetype::Alpine: return "Alpine";
    case Archetype::Canyon: return "Canyon";
    case Archetype::HighlandPlateau: return "HighlandPlateau";
    case Archetype::RegolithPlains: return "RegolithPlains";
    case Archetype::Cratered: return "Cratered";
    case Archetype::Highlands: return "Highlands";
    case Archetype::Dunes: return "Dunes";
    case Archetype::Mesas: return "Mesas";
    case Archetype::Canyonlands: return "Canyonlands";
    case Archetype::GlacialShield: return "GlacialShield";
    case Archetype::CrevasseField: return "CrevasseField";
    case Archetype::RidgeField: return "RidgeField";
  }
  return "?";
}

ProvinceField::ProvinceField(const core::Key& body_key, const PlanetParams& planet)
    : provinces_key_(core::derive_named(body_key, core::NameId::ProvincesV1)),
      type_(planet.type),
      cells_per_face_(planet.cells_per_face) {}

ProvinceParams ProvinceField::cell_params(const CellId& cell) const {
  const core::Key cell_key = core::derive_child(provinces_key_, core::Kind::Province, cell.face,
                                                cell.ci, cell.cj);
  const auto draw0 = core::draw_point(cell_key, core::Channel::Params, 0, 0, 0);
  const auto draw1 = core::draw_point(cell_key, core::Channel::Params, 1, 0, 0);

  const ArchetypeSpec& spec = weighted_pick(table_for(type_), draw0[2]);
  ProvinceParams params;
  params.archetype = spec.archetype;
  params.relief_amplitude_m = uniform(draw1[0], spec.relief_lo, spec.relief_hi);
  params.base_elevation_m = uniform(draw1[1], spec.base_lo, spec.base_hi);
  params.ruggedness = uniform(draw1[2], spec.rug_lo, spec.rug_hi);
  params.carving = uniform(draw1[3], spec.carve_lo, spec.carve_hi);
  params.palette_shift = static_cast<std::uint32_t>(draw0[3] >> 40U);
  return params;
}

CellId ProvinceField::cell_of(const Dir3& unit_dir) const {
  const FaceUV face_uv = dir_to_face_uv(unit_dir);
  const auto n = static_cast<double>(cells_per_face_);
  const double fu = (face_uv.u.to_double() + 1.0) * 0.5 * n;
  const double fv = (face_uv.v.to_double() + 1.0) * 0.5 * n;
  auto clamp_cell = [&](double value) {
    if (value < 0.0) return std::uint32_t{0};
    if (value >= n) return cells_per_face_ - 1;
    return static_cast<std::uint32_t>(value);
  };
  return CellId{face_uv.face, clamp_cell(fu), clamp_cell(fv)};
}

Dir3 ProvinceField::representative(const CellId& cell) const {
  const core::Key cell_key = core::derive_child(provinces_key_, core::Kind::Province, cell.face,
                                                cell.ci, cell.cj);
  const auto draw0 = core::draw_point(cell_key, core::Channel::Params, 0, 0, 0);
  const Real n(static_cast<double>(cells_per_face_));
  // Jitter within +-0.4 cells of the cell center.
  const Real ju = uniform(draw0[0], -0.4, 0.4);
  const Real jv = uniform(draw0[1], -0.4, 0.4);
  const Real u = (Real(static_cast<double>(cell.ci)) + Real(0.5) + ju) / n * Real(2.0) - Real(1.0);
  const Real v = (Real(static_cast<double>(cell.cj)) + Real(0.5) + jv) / n * Real(2.0) - Real(1.0);
  return face_uv_to_dir(FaceUV{cell.face, u, v});
}

BlendedParams ProvinceField::sample(const Dir3& unit_dir) const {
  const auto n = static_cast<double>(cells_per_face_);
  // Finite-support kernel radius in chord units: must exceed the largest
  // possible nearest-representative distance (~2.55/N) so total weight is
  // never zero. Continuity then requires the candidate stencil to contain
  // EVERY cell whose representative lies within this radius — weights
  // reach exactly zero strictly inside the candidate set.
  const Real radius(2.6 / n);
  const Real radius_sq = radius * radius;
  // Probe spacing must stay below the smallest cell chord size anywhere on
  // the cube-sphere (~0.94/N near corners, where the uv->sphere metric
  // compresses to ~0.47): 0.6/N guarantees no cell inside the reach disc
  // can slip between diagonal probes (0.6*sqrt(2) = 0.85 < 0.94). Reach
  // +-7 steps = 4.2/N chord > radius + one-cell margin, with headroom for
  // the slight chord compression of larger tangent offsets.
  const Real step(0.6 / n);
  constexpr int kStencilHalf = 7;

  Dir3 t1{};
  Dir3 t2{};
  tangent_basis(unit_dir, &t1, &t2);

  std::set<CellId> candidates;
  for (int di = -kStencilHalf; di <= kStencilHalf; ++di) {
    for (int dj = -kStencilHalf; dj <= kStencilHalf; ++dj) {
      const Real offset_u = step * Real(static_cast<double>(di));
      const Real offset_v = step * Real(static_cast<double>(dj));
      const Dir3 probe = normalize(Dir3{unit_dir.x + t1.x * offset_u + t2.x * offset_v,
                                        unit_dir.y + t1.y * offset_u + t2.y * offset_v,
                                        unit_dir.z + t1.z * offset_u + t2.z * offset_v});
      candidates.insert(cell_of(probe));
    }
  }

  BlendedParams blended{};
  Real total_weight(0.0);
  Real best_weight(-1.0);
  for (const CellId& cell : candidates) {
    const Dir3 rep = representative(cell);
    const Real dist_sq = chord_sq(unit_dir, rep);
    if (dist_sq >= radius_sq) {
      continue;
    }
    const Real falloff = (radius_sq - dist_sq) / radius_sq;
    const Real weight = falloff * falloff * falloff;
    const ProvinceParams params = cell_params(cell);
    blended.relief_amplitude_m += weight * params.relief_amplitude_m;
    blended.base_elevation_m += weight * params.base_elevation_m;
    blended.ruggedness += weight * params.ruggedness;
    blended.carving += weight * params.carving;
    total_weight += weight;
    if (weight > best_weight) {
      best_weight = weight;
      blended.dominant = cell;
      blended.dominant_archetype = params.archetype;
    }
  }

  if (total_weight > Real(0.0)) {
    blended.relief_amplitude_m = blended.relief_amplitude_m / total_weight;
    blended.base_elevation_m = blended.base_elevation_m / total_weight;
    blended.ruggedness = blended.ruggedness / total_weight;
    blended.carving = blended.carving / total_weight;
  } else {
    // Coverage guarantee should make this unreachable; deterministic
    // fallback to the owner cell keeps the function total regardless.
    const CellId owner = cell_of(unit_dir);
    const ProvinceParams params = cell_params(owner);
    blended.relief_amplitude_m = params.relief_amplitude_m;
    blended.base_elevation_m = params.base_elevation_m;
    blended.ruggedness = params.ruggedness;
    blended.carving = params.carving;
    blended.dominant = owner;
    blended.dominant_archetype = params.archetype;
  }
  return blended;
}

std::vector<CellId> ProvinceField::all_cells() const {
  std::vector<CellId> cells;
  cells.reserve(6U * cells_per_face_ * cells_per_face_);
  for (std::uint8_t face = 0; face < 6; ++face) {
    for (std::uint32_t ci = 0; ci < cells_per_face_; ++ci) {
      for (std::uint32_t cj = 0; cj < cells_per_face_; ++cj) {
        cells.push_back(CellId{face, ci, cj});
      }
    }
  }
  return cells;
}

std::string ProvinceField::table_to_json() const {
  std::string out = "[\n";
  const std::vector<CellId> cells = all_cells();
  for (std::size_t i = 0; i < cells.size(); ++i) {
    const ProvinceParams params = cell_params(cells[i]);
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "  {\"face\": %u, \"ci\": %u, \"cj\": %u, \"archetype\": \"%s\", "
                  "\"relief_m\": %.2f, \"base_m\": %.2f, \"ruggedness\": %.4f, "
                  "\"carving\": %.4f, \"palette_shift\": %u}%s\n",
                  cells[i].face, cells[i].ci, cells[i].cj, to_string(params.archetype),
                  params.relief_amplitude_m.to_double(), params.base_elevation_m.to_double(),
                  params.ruggedness.to_double(), params.carving.to_double(),
                  params.palette_shift, i + 1 < cells.size() ? "," : "");
    out += buffer;
  }
  out += "]\n";
  return out;
}

}  // namespace inf::gen
