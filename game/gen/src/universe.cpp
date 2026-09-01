#include "gen/universe.hpp"

#include "core/det/trig.hpp"

namespace inf::gen {

using core::tree::AxisDesc;
using core::tree::Cell;
using core::tree::GeneratorRegistry;
using core::tree::NodeSpec;
using core::tree::Step;
using core::tree::Topology;

namespace {

// Structural nodes carry no params yet; their axes are the content.
NodeSpec universe_spec(const core::Key&, const core::tree::Node*) {
  NodeSpec spec;
  AxisDesc clusters;
  clusters.name = name::ClustersAxis;
  clusters.child_kind = kind::Cluster;
  clusters.topo = Topology::CellGrid3D;  // unbounded, anchored at (0,0,0)
  spec.axes = {clusters};
  return spec;
}

NodeSpec cluster_spec(const core::Key& entity_key, const core::tree::Node*) {
  NodeSpec spec;
  AxisDesc galaxies;
  galaxies.name = name::GalaxiesAxis;
  galaxies.child_kind = kind::Galaxy;
  galaxies.topo = Topology::IndexedList;
  // Real galaxy counts (T0017 WP5): 10-1000 per cluster, drawn from
  // galaxy-layout/v1 off the cluster entity; positions come from the
  // same layer (galaxy_position_in_cluster).
  galaxies.count = [entity_key](const core::tree::Node&, const core::Key&) {
    return static_cast<std::uint64_t>(galaxy_count_in_cluster(entity_key));
  };
  galaxies.occupied = [entity_key](const core::tree::Node&, const core::Key&,
                                   const Cell& cell) {
    return cell.x >= 0 &&
           cell.x < static_cast<std::int64_t>(galaxy_count_in_cluster(entity_key));
  };
  spec.axes = {galaxies};
  return spec;
}

NodeSpec galaxy_spec(const core::Key&, const core::tree::Node*) {
  NodeSpec spec;
  AxisDesc systems;
  systems.name = name::SystemsAxis;
  systems.child_kind = kind::System;
  // Real octree cells (T0017 WP3): (x, y, z, level) with the level in
  // cell.w, level 0 the root cube. Cell (0,0,0,0) stays the DEFAULT
  // (home) system — its key predates the octree and must not move. True
  // occupancy is GalaxyOctree's density-driven draw; like the planet
  // slots, the axis only bounds the coordinate space and callers hold
  // the occupancy contract.
  systems.topo = Topology::Octree;
  systems.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    if (cell.w < 0 || cell.w > 18) {
      return false;
    }
    const std::int64_t extent = std::int64_t{1} << cell.w;
    return cell.x >= 0 && cell.x < extent && cell.y >= 0 && cell.y < extent &&
           cell.z >= 0 && cell.z < extent;
  };
  // Deep-sky entities (T0017 WP4): coarse placement grids; real occupancy
  // is the arm-weighted draw in NebulaField / StarClusterField.
  AxisDesc nebulae;
  nebulae.name = name::NebulaeV1;
  nebulae.child_kind = kind::Nebula;
  nebulae.topo = Topology::CellGrid3D;
  nebulae.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x >= 0 && cell.x < 32 && cell.y >= 0 && cell.y < 32 && cell.z >= 0 &&
           cell.z < 32;
  };
  AxisDesc star_clusters;
  star_clusters.name = name::StarClustersV1;
  star_clusters.child_kind = kind::StarCluster;
  star_clusters.topo = Topology::CellGrid3D;
  star_clusters.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x >= 0 && cell.x < 32 && cell.y >= 0 && cell.y < 32 && cell.z >= 0 &&
           cell.z < 32;
  };
  spec.axes = {systems, nebulae, star_clusters};
  return spec;
}

NodeSpec system_spec(const core::Key&, const core::tree::Node*) {
  NodeSpec spec;
  AxisDesc planets;
  planets.name = name::PlanetsAxis;
  planets.child_kind = kind::Body;
  planets.topo = Topology::SlotTable;
  // Occupancy comes from "architecture/v1" once T0012 gates it; until the
  // renderer consumes system layouts, every slot in range materializes.
  planets.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x >= 0 && cell.x < 16;
  };
  AxisDesc stars;
  stars.name = name::StarsAxis;
  stars.child_kind = kind::Star;
  stars.topo = Topology::SlotTable;
  stars.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x == 0;  // single-star gate (multi-star structures ready)
  };
  AxisDesc belts;
  belts.name = name::BeltsAxis;
  belts.child_kind = kind::Belt;
  belts.topo = Topology::SlotTable;
  spec.axes = {planets, stars, belts};
  return spec;
}

NodeSpec leaf_spec(const core::Key&, const core::tree::Node*) {
  return NodeSpec{};  // params drawn by the layer code from the node keys
}

// Planets/moons: a Body owns a moons axis (moons are full bodies with
// their own layer keys — landable like planets, T0016). Adding the axis
// is extension-safe: axis keys hang under childrenKey, the body's own
// entity/params keys are untouched.
NodeSpec body_spec(const core::Key&, const core::tree::Node*) {
  NodeSpec spec;
  AxisDesc moons;
  moons.name = name::MoonsAxis;
  moons.child_kind = kind::Body;
  moons.topo = Topology::SlotTable;
  // Occupancy is enforced by the caller against moons/v1's drawn count.
  moons.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x >= 0 && cell.x < 16;
  };
  // Caves are addressable entities too (T0015 WP7): stable keys today,
  // children (poi/resources/structures) later. Real occupancy is the
  // caves/v1 probability test, enforced by CaveField.
  AxisDesc caves;
  caves.name = name::CavesV1;
  caves.child_kind = kind::Cave;
  caves.topo = Topology::CellGrid3D;
  caves.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x >= 0 && cell.x < 6 && cell.y >= 0 && cell.z >= 0;
  };
  spec.axes = {moons, caves};
  return spec;
}

}  // namespace

GeneratorRegistry make_registry() {
  GeneratorRegistry registry;
  registry.register_kind(kind::Universe, universe_spec);
  registry.register_kind(kind::Cluster, cluster_spec);
  registry.register_kind(kind::Galaxy, galaxy_spec);
  registry.register_kind(kind::System, system_spec);
  registry.register_kind(kind::Body, body_spec);
  registry.register_kind(kind::Star, leaf_spec);
  registry.register_kind(kind::Belt, leaf_spec);
  registry.register_kind(kind::Barycenter, leaf_spec);
  return registry;
}

std::unique_ptr<core::tree::InfinityTree> make_tree(const core::Seed128& seed) {
  return std::make_unique<core::tree::InfinityTree>(seed, kind::Universe, make_registry());
}

core::tree::Address default_system_address() {
  return core::tree::Address{}
      .child(Step{name::ClustersAxis, Cell::grid(0, 0, 0)})
      .child(Step{name::GalaxiesAxis, Cell::index(0)})
      .child(Step{name::SystemsAxis, Cell::grid(0, 0, 0)});
}

core::tree::Address default_body_address() {
  return default_system_address().child(Step{name::PlanetsAxis, Cell::slot(0)});
}

BodyHandle default_body(const core::Seed128& seed) {
  const auto tree = make_tree(seed);
  const auto node = tree->get(default_body_address());
  // The default path is always occupied by construction.
  return BodyHandle{node->key(), node->params_key()};
}

core::Key default_system_key(const core::Seed128& seed) {
  const auto tree = make_tree(seed);
  return tree->get(default_system_address())->key();
}

BodyHandle body_for_slot(const core::Seed128& seed, int slot) {
  const auto tree = make_tree(seed);
  const auto address =
      default_system_address().child(Step{name::PlanetsAxis, Cell::slot(slot)});
  const auto node = tree->get(address);
  return BodyHandle{node->key(), node->params_key()};
}

BodyHandle body_for_moon(const core::Seed128& seed, int slot, int moon_index) {
  const auto tree = make_tree(seed);
  const auto address = default_system_address()
                           .child(Step{name::PlanetsAxis, Cell::slot(slot)})
                           .child(Step{name::MoonsAxis, Cell::slot(moon_index)});
  const auto node = tree->get(address);
  return BodyHandle{node->key(), node->params_key()};
}

PlanetParams derive_planet_params(const BodyHandle& body,
                                  std::optional<PlanetType> forced_type) {
  return derive_planet_params(body.entity, body.params, forced_type);
}

core::tree::Address system_address_for(const SystemCell& cell) {
  Cell tree_cell;
  tree_cell.x = cell.x;
  tree_cell.y = cell.y;
  tree_cell.z = cell.z;
  tree_cell.w = cell.level;
  return core::tree::Address{}
      .child(Step{name::ClustersAxis, Cell::grid(0, 0, 0)})
      .child(Step{name::GalaxiesAxis, Cell::index(0)})
      .child(Step{name::SystemsAxis, tree_cell});
}

core::Key system_key_for(const core::Seed128& seed, const SystemCell& cell) {
  const auto tree = make_tree(seed);
  return tree->get(system_address_for(cell))->key();
}

BodyHandle body_for_system_slot(const core::Seed128& seed, const SystemCell& cell,
                                int slot) {
  const auto tree = make_tree(seed);
  const auto address =
      system_address_for(cell).child(Step{name::PlanetsAxis, Cell::slot(slot)});
  const auto node = tree->get(address);
  return BodyHandle{node->key(), node->params_key()};
}

BodyHandle body_for_system_moon(const core::Seed128& seed, const SystemCell& cell,
                                int slot, int moon_index) {
  const auto tree = make_tree(seed);
  const auto address = system_address_for(cell)
                           .child(Step{name::PlanetsAxis, Cell::slot(slot)})
                           .child(Step{name::MoonsAxis, Cell::slot(moon_index)});
  const auto node = tree->get(address);
  return BodyHandle{node->key(), node->params_key()};
}

std::uint32_t galaxy_count_in_cluster(const core::Key& cluster_entity_key) {
  const core::Key layout = core::derive_named(cluster_entity_key, name::GalaxyLayoutV1);
  const auto draw = core::draw_point(layout, channel::Params, 0, 0, 0);
  // Log-uniform 10-1000: 10 * 100^u.
  const double u = static_cast<double>(draw[0] >> 11U) * 0x1.0p-53;
  const double count =
      10.0 * det::fast_exp(det::Real(u * 4.605170185988091)).to_double();
  const auto result = static_cast<std::uint32_t>(count);
  return result < 10U ? 10U : (result > 1000U ? 1000U : result);
}

Dir3 galaxy_position_in_cluster(const core::Key& cluster_entity_key, std::uint32_t index) {
  if (index == 0) {
    // The home galaxy of every cluster anchors its origin; for the HOME
    // cluster this keeps the playable galaxy exactly where it was.
    return Dir3{det::Real(0.0), det::Real(0.0), det::Real(0.0)};
  }
  const core::Key layout = core::derive_named(cluster_entity_key, name::GalaxyLayoutV1);
  const auto draw = core::draw_point(layout, channel::Params, index, 1, 0);
  const auto coord = [&](std::uint64_t word) {
    const double u = static_cast<double>(word >> 11U) * 0x1.0p-53;
    return det::Real((u - 0.5) * kClusterSizeM);
  };
  return Dir3{coord(draw[0]), coord(draw[1]), coord(draw[2])};
}

core::Key galaxy_key_in_cluster(const core::Seed128& seed, std::int64_t cx,
                                std::int64_t cy, std::int64_t cz, std::uint32_t index) {
  const auto tree = make_tree(seed);
  const auto address =
      core::tree::Address{}
          .child(Step{name::ClustersAxis, Cell::grid(cx, cy, cz)})
          .child(Step{name::GalaxiesAxis, Cell::index(static_cast<std::int64_t>(index))});
  return tree->get(address)->key();
}

core::Key home_galaxy_key(const core::Seed128& seed) {
  const auto tree = make_tree(seed);
  const auto address =
      core::tree::Address{}
          .child(Step{name::ClustersAxis, Cell::grid(0, 0, 0)})
          .child(Step{name::GalaxiesAxis, Cell::index(0)});
  return tree->get(address)->key();
}

GalaxyParams home_galaxy_params(const core::Seed128& seed) {
  // Morphology forced to Barred (see the header note); all other draws
  // stay keyed off the galaxy entity as usual.
  return derive_galaxy_params(home_galaxy_key(seed), GalaxyType::Barred);
}

}  // namespace inf::gen
