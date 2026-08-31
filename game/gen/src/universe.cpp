#include "gen/universe.hpp"

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

NodeSpec cluster_spec(const core::Key&, const core::tree::Node*) {
  NodeSpec spec;
  AxisDesc galaxies;
  galaxies.name = name::GalaxiesAxis;
  galaxies.child_kind = kind::Galaxy;
  galaxies.topo = Topology::IndexedList;
  galaxies.count = [](const core::tree::Node&, const core::Key&) { return std::uint64_t{1}; };
  galaxies.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x == 0;  // one galaxy per cluster until galaxy gen lands
  };
  spec.axes = {galaxies};
  return spec;
}

NodeSpec galaxy_spec(const core::Key&, const core::tree::Node*) {
  NodeSpec spec;
  AxisDesc systems;
  systems.name = name::SystemsAxis;
  systems.child_kind = kind::System;
  // Octree per the design; interface-stubbed as an indexed list until the
  // galaxy population layer exists (T0011).
  systems.topo = Topology::Octree;
  systems.count = [](const core::tree::Node&, const core::Key&) { return std::uint64_t{1}; };
  systems.occupied = [](const core::tree::Node&, const core::Key&, const Cell& cell) {
    return cell.x == 0 && cell.y == 0 && cell.z == 0 && cell.w == 0;
  };
  spec.axes = {systems};
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
  spec.axes = {moons};
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

}  // namespace inf::gen
