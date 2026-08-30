#include <doctest/doctest.h>

#include "core/tree/tree.hpp"

using namespace inf::core;
using namespace inf::core::tree;

namespace {

constexpr KindId kRootKind{100};
constexpr KindId kLeafKind{101};
constexpr NameId kListAxis{0x1111111111111111ULL};
constexpr NameId kGridAxis{0x2222222222222222ULL};

struct RootParams {
  std::uint64_t flavor;
};
struct LeafParams {
  std::uint64_t value;
};

GeneratorRegistry make_registry() {
  GeneratorRegistry registry;
  registry.register_kind(kRootKind, [](const Key& params_key, const Node*) {
    NodeSpec spec;
    spec.params = RootParams{params_key.k0};
    AxisDesc list;
    list.name = kListAxis;
    list.child_kind = kLeafKind;
    list.topo = Topology::IndexedList;
    list.count = [](const Node&, const Key&) { return std::uint64_t{3}; };
    list.occupied = [](const Node&, const Key&, const Cell& cell) {
      return cell.x >= 0 && cell.x < 3;
    };
    AxisDesc grid;
    grid.name = kGridAxis;
    grid.child_kind = kLeafKind;
    grid.topo = Topology::CellGrid3D;
    spec.axes = {list, grid};
    return spec;
  });
  registry.register_kind(kLeafKind, [](const Key& params_key, const Node* parent) {
    NodeSpec spec;
    // One-directional parent read: fold the parent's flavor in.
    const std::uint64_t base = parent != nullptr ? parent->params<RootParams>().flavor : 0;
    spec.params = LeafParams{params_key.k0 ^ base};
    return spec;
  });
  return registry;
}

Address leaf_at(std::int64_t i) {
  return Address{}.child(Step{kListAxis, Cell::index(i)});
}

}  // namespace

TEST_CASE("tree: two-seed rule separates params from subtree") {
  InfinityTree tree(Seed128{1, 2}, kRootKind, make_registry());
  const auto root = tree.get(Address{});
  REQUIRE(root != nullptr);
  CHECK(root->params_key() == derive_named(root->key(), kParamsName));
  CHECK(root->children_key() == derive_named(root->key(), kChildrenName));
  CHECK(!(root->params_key() == root->children_key()));

  // Axis keys hang under childrenKey; entry keys under the axis.
  const auto axis = root->axis(kListAxis);
  REQUIRE(axis.has_value());
  CHECK(axis->axis_key() == derive_named(root->children_key(), kListAxis));
}

TEST_CASE("tree: materialize-anywhere, no order dependence") {
  InfinityTree tree_a(Seed128{7, 9}, kRootKind, make_registry());
  InfinityTree tree_b(Seed128{7, 9}, kRootKind, make_registry());

  // Far grid cell materialized FIRST on tree_a, LAST on tree_b: identical.
  const Address far = Address{}.child(Step{kGridAxis, Cell::grid(1'000'000'000'000LL,
                                                                -42, 3)});
  const auto far_a = tree_a.get(far);
  const auto near_b = tree_b.get(leaf_at(0));
  const auto far_b = tree_b.get(far);
  REQUIRE(far_a != nullptr);
  REQUIRE(far_b != nullptr);
  CHECK(far_a->key() == far_b->key());
  CHECK(far_a->params<LeafParams>().value == far_b->params<LeafParams>().value);
  CHECK(near_b != nullptr);
}

TEST_CASE("tree: occupancy bounds and cache purity") {
  InfinityTree tree(Seed128{3, 4}, kRootKind, make_registry(), nullptr,
                    TreeConfig{.cache_capacity = 2});
  CHECK(tree.get(leaf_at(2)) != nullptr);
  CHECK(tree.get(leaf_at(3)) == nullptr);  // out of the occupied range

  // Cache eviction must not change results (purity).
  const auto first = tree.get(leaf_at(0));
  const auto key_before = first->key();
  tree.get(leaf_at(1));
  tree.get(leaf_at(2));  // capacity 2: leaf 0 evicted by now
  tree.clear_cache();
  const auto again = tree.get(leaf_at(0));
  CHECK(again->key() == key_before);
}

TEST_CASE("tree: SeedPin inception overrides a subtree's key") {
  const Address graft = leaf_at(1);
  const Key pinned{0xDEAD, 0xBEEF};
  auto store = std::make_shared<InceptionStore>(
      std::vector<Inception>{Inception{graft, SeedPin{pinned}}});

  InfinityTree plain(Seed128{5, 6}, kRootKind, make_registry());
  InfinityTree grafted(Seed128{5, 6}, kRootKind, make_registry(), store);

  CHECK(!(plain.get(graft)->key() == pinned));
  CHECK(grafted.get(graft)->key() == pinned);
  // Sibling unaffected (isolation makes grafting safe).
  CHECK(grafted.get(leaf_at(0))->key() == plain.get(leaf_at(0))->key());

  // Runtime swap: removing the graft restores procedural content.
  grafted.set_inceptions(nullptr);
  CHECK(grafted.get(graft)->key() == plain.get(graft)->key());
}
