#pragma once

// The InfinityTree (design/infinity-tree.md): the universe's address
// space plus the one stateful materializer. The tree is NEVER stored —
// address = identity = seed source = diff key; coordinate -> address ->
// keys is closed-form (materialize-anywhere is a hard requirement).
// Node objects are transient, immutable materializations (flyweights in
// an LRU cache); evicting everything and re-materializing is
// observationally identical.

#include <any>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/key.hpp"

namespace inf::core::tree {

// Framework-level name ids (first 8 bytes LE of MD5, frozen; verified by
// engine tests): the two-seed rule's derivation names.
inline constexpr NameId kParamsName{0xccc86c8a5bceff21ULL};    // "params"
inline constexpr NameId kChildrenName{0xf527f02dc1848126ULL};  // "children"

// ---- identity ------------------------------------------------------------

// Where on an axis an entry sits. Index/slot use x; grids/octrees use
// x,y,z (level for octrees goes in w).
struct Cell {
  std::int64_t x{0};
  std::int64_t y{0};
  std::int64_t z{0};
  std::int32_t w{0};  // octree level; 0 otherwise

  static constexpr Cell index(std::int64_t i) { return Cell{i, 0, 0, 0}; }
  static constexpr Cell slot(std::int64_t s) { return Cell{s, 0, 0, 0}; }
  static constexpr Cell grid(std::int64_t x, std::int64_t y, std::int64_t z) {
    return Cell{x, y, z, 0};
  }
  friend bool operator==(const Cell&, const Cell&) = default;
};

// One step down the tree: which axis, and where on it.
struct Step {
  NameId axis{};
  Cell at{};
  friend bool operator==(const Step&, const Step&) = default;
};

// Path from the universe root. Identity, seed source, diff key.
class Address {
 public:
  Address() = default;
  explicit Address(std::vector<Step> steps) : steps_(std::move(steps)) {}

  bool is_root() const { return steps_.empty(); }
  std::size_t depth() const { return steps_.size(); }
  std::span<const Step> steps() const { return steps_; }
  const Step& last() const { return steps_.back(); }

  Address parent() const {
    Address up = *this;
    up.steps_.pop_back();
    return up;
  }
  Address child(Step step) const {
    Address down = *this;
    down.steps_.push_back(step);
    return down;
  }
  // True if `this` is a prefix of (or equal to) other.
  bool is_prefix_of(const Address& other) const;

  std::string str() const;
  std::uint64_t hash() const;
  friend bool operator==(const Address&, const Address&) = default;

 private:
  std::vector<Step> steps_;
};

struct AddressHash {
  std::size_t operator()(const Address& a) const { return static_cast<std::size_t>(a.hash()); }
};

// ---- axes ----------------------------------------------------------------

enum class Topology : std::uint8_t {
  IndexedList = 0,  // count-bounded, index-keyed
  SlotTable = 1,    // fixed slot space, meaningful empty slots
  CellGrid3D = 2,   // unbounded sparse integer grid
  Octree = 3,       // implicit spatial octree (interface stub for now)
};

class Node;

struct AxisDesc {
  NameId name{};            // stable, versioned ("systems/v1")
  KindId child_kind{};      // what it produces
  Topology topo{Topology::IndexedList};
  bool diffable{false};
  // Occupancy: is there an entry at this cell? Pure function of the axis
  // key + cell (+ the owning node's params via the captured node). Null =
  // always occupied (within bounds the game enforces itself).
  std::function<bool(const Node& owner, const Key& axis_key, const Cell&)> occupied;
  // Entry count for IndexedList/SlotTable enumeration (null = unbounded /
  // not enumerable).
  std::function<std::uint64_t(const Node& owner, const Key& axis_key)> count;
};

// Lazy handle onto one axis of a materialized node. Returns ADDRESSES —
// no RNG state ever crosses an axis boundary; children re-derive
// everything from their address.
class AxisView {
 public:
  AxisView(const Node& owner, const AxisDesc& desc, Key axis_key)
      : owner_(owner), desc_(desc), axis_key_(axis_key) {}

  const Key& axis_key() const { return axis_key_; }
  const AxisDesc& desc() const { return desc_; }

  // Entry key for a cell (derivable without materializing anything).
  Key entry_key(const Cell& cell) const {
    return derive_child(axis_key_, desc_.child_kind, cell.x, cell.y, cell.z);
  }

  std::optional<Address> at(const Cell& cell) const;
  std::uint64_t count() const;

 private:
  const Node& owner_;
  const AxisDesc& desc_;
  Key axis_key_;
};

// ---- node ----------------------------------------------------------------

// Generic kind-driven node (DECISIONS: no per-kind subclasses; generators
// registered per kind produce a serializable param payload + axis table).
class Node {
 public:
  Node(Address address, Key key, KindId kind, std::any params,
       std::vector<AxisDesc> axes)
      : address_(std::move(address)),
        key_(key),
        kind_(kind),
        params_(std::move(params)),
        axes_(std::move(axes)) {}

  const Address& address() const { return address_; }
  const Key& key() const { return key_; }
  // Two-seed rule (design/infinity-tree.md section 1): params and subtree
  // randomness are separated by construction.
  Key params_key() const { return derive_named(key_, tree::kParamsName); }
  Key children_key() const { return derive_named(key_, tree::kChildrenName); }
  KindId kind() const { return kind_; }

  template <typename T>
  const T& params() const {
    return *std::any_cast<T>(&params_);
  }
  bool has_params() const { return params_.has_value(); }

  std::span<const AxisDesc> axes() const { return axes_; }
  // Opens an axis by name; the axis key hangs under children_key().
  std::optional<AxisView> axis(NameId name) const;

 private:
  Address address_;
  Key key_;
  KindId kind_;
  std::any params_;
  std::vector<AxisDesc> axes_;
};

// ---- generators ----------------------------------------------------------

struct NodeSpec {
  std::any params;             // serializable per-kind payload
  std::vector<AxisDesc> axes;  // typed child axes
};

// Generates a node of one kind: draws params from params_key (never the
// children key — two-seed rule enforced by what is passed), reads the
// parent's published outputs one-directionally.
using Generator =
    std::function<NodeSpec(const Key& params_key, const Node* parent)>;

class GeneratorRegistry {
 public:
  void register_kind(KindId kind, Generator generator) {
    generators_[static_cast<std::uint32_t>(kind)] = std::move(generator);
  }
  const Generator* find(KindId kind) const {
    const auto it = generators_.find(static_cast<std::uint32_t>(kind));
    return it == generators_.end() ? nullptr : &it->second;
  }

 private:
  std::unordered_map<std::uint32_t, Generator> generators_;
};

// ---- inception overlay ---------------------------------------------------

// Authored grafts (design/infinity-tree.md section 5): third world-state
// tier between the procedural base and the player diff. SeedPin works;
// KindSwap/AuthoredRef are stubbed behind the variant until needed.
struct SeedPin {
  Key pinned_key;
};
struct KindSwap {
  KindId kind;  // stub: alternate generator (not yet honored)
};
struct AuthoredRef {
  std::uint64_t asset_id;  // stub: stored-content generator (not yet honored)
};

struct Inception {
  Address root;
  std::variant<SeedPin, KindSwap, AuthoredRef> what;
};

class InceptionStore {
 public:
  explicit InceptionStore(std::vector<Inception> entries = {});
  // Longest-prefix match: the innermost graft whose root prefixes addr.
  const Inception* match(const Address& addr) const;
  // Exact-root lookup (materializer applies overrides at the graft root).
  const Inception* at_root(const Address& addr) const;
  std::span<const Inception> entries() const { return entries_; }

 private:
  std::vector<Inception> entries_;
};

// ---- the tree ------------------------------------------------------------

struct TreeConfig {
  std::size_t cache_capacity = 4096;
};

// THE tree: the only stateful object. Pure function address -> node,
// memoized in an LRU cache; caching is an optimization, never a semantic.
class InfinityTree {
 public:
  InfinityTree(const Seed128& seed, KindId root_kind, GeneratorRegistry registry,
               std::shared_ptr<const InceptionStore> inceptions = {},
               TreeConfig config = {});

  // Materialize (or fetch cached) the node at an address. Returns nullptr
  // for unoccupied/unresolvable addresses.
  std::shared_ptr<const Node> get(const Address& address);

  // Swap the inception overlay at runtime: atomic snapshot exchange plus
  // prefix invalidation of cached nodes under the grafted roots.
  void set_inceptions(std::shared_ptr<const InceptionStore> store);

  const GeneratorRegistry& registry() const { return registry_; }

  std::size_t cached_nodes() const { return cache_.size(); }
  void clear_cache();

 private:
  std::shared_ptr<const Node> materialize(const Address& address);
  void invalidate_prefix(const Address& prefix);
  void touch(const Address& address);

  Seed128 seed_;
  KindId root_kind_;
  GeneratorRegistry registry_;
  std::shared_ptr<const InceptionStore> inceptions_;
  TreeConfig config_;

  struct CacheEntry {
    std::shared_ptr<const Node> node;
    std::list<Address>::iterator lru_it;
  };
  std::unordered_map<Address, CacheEntry, AddressHash> cache_;
  std::list<Address> lru_;  // front = most recent
};

}  // namespace inf::core::tree
