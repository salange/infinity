#include "core/tree/tree.hpp"

#include <algorithm>
#include <cstdio>

#include "core/det/mix.hpp"

namespace inf::core::tree {

// ---- Address -------------------------------------------------------------

bool Address::is_prefix_of(const Address& other) const {
  if (steps_.size() > other.steps_.size()) {
    return false;
  }
  for (std::size_t i = 0; i < steps_.size(); ++i) {
    if (!(steps_[i] == other.steps_[i])) {
      return false;
    }
  }
  return true;
}

std::string Address::str() const {
  std::string out = "/";
  for (const Step& step : steps_) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%016llx(%lld,%lld,%lld,%d)/",
                  static_cast<unsigned long long>(step.axis),
                  static_cast<long long>(step.at.x), static_cast<long long>(step.at.y),
                  static_cast<long long>(step.at.z), step.at.w);
    out += buffer;
  }
  return out;
}

std::uint64_t Address::hash() const {
  std::uint64_t h = 0x9E3779B97F4A7C15ULL;
  for (const Step& step : steps_) {
    h = det::mix64(h ^ static_cast<std::uint64_t>(step.axis));
    h = det::mix64(h ^ static_cast<std::uint64_t>(step.at.x));
    h = det::mix64(h ^ static_cast<std::uint64_t>(step.at.y));
    h = det::mix64(h ^ static_cast<std::uint64_t>(step.at.z));
    h = det::mix64(h ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(step.at.w)));
  }
  return h;
}

// ---- AxisView / Node -----------------------------------------------------

std::optional<Address> AxisView::at(const Cell& cell) const {
  if (desc_.occupied && !desc_.occupied(owner_, axis_key_, cell)) {
    return std::nullopt;
  }
  return owner_.address().child(Step{desc_.name, cell});
}

std::uint64_t AxisView::count() const {
  if (!desc_.count) {
    return 0;
  }
  return desc_.count(owner_, axis_key_);
}

std::optional<AxisView> Node::axis(NameId name) const {
  for (const AxisDesc& desc : axes_) {
    if (desc.name == name) {
      return AxisView(*this, desc, derive_named(children_key(), desc.name));
    }
  }
  return std::nullopt;
}

// ---- InceptionStore ------------------------------------------------------

InceptionStore::InceptionStore(std::vector<Inception> entries)
    : entries_(std::move(entries)) {}

const Inception* InceptionStore::match(const Address& addr) const {
  const Inception* best = nullptr;
  for (const Inception& entry : entries_) {
    if (entry.root.is_prefix_of(addr)) {
      if (best == nullptr || entry.root.depth() > best->root.depth()) {
        best = &entry;
      }
    }
  }
  return best;
}

const Inception* InceptionStore::at_root(const Address& addr) const {
  for (const Inception& entry : entries_) {
    if (entry.root == addr) {
      return &entry;
    }
  }
  return nullptr;
}

// ---- InfinityTree --------------------------------------------------------

InfinityTree::InfinityTree(const Seed128& seed, KindId root_kind,
                           GeneratorRegistry registry,
                           std::shared_ptr<const InceptionStore> inceptions,
                           TreeConfig config)
    : seed_(seed),
      root_kind_(root_kind),
      registry_(std::move(registry)),
      inceptions_(std::move(inceptions)),
      config_(config) {}

std::shared_ptr<const Node> InfinityTree::get(const Address& address) {
  const auto it = cache_.find(address);
  if (it != cache_.end()) {
    touch(address);
    return it->second.node;
  }
  auto node = materialize(address);
  if (node != nullptr) {
    lru_.push_front(address);
    cache_.emplace(address, CacheEntry{node, lru_.begin()});
    while (cache_.size() > config_.cache_capacity) {
      const Address& victim = lru_.back();
      cache_.erase(victim);
      lru_.pop_back();
    }
  }
  return node;
}

std::shared_ptr<const Node> InfinityTree::materialize(const Address& address) {
  Key key = universe_key(seed_);
  KindId kind = root_kind_;
  std::shared_ptr<const Node> parent;

  // Root inception (SeedPin at the empty address).
  const auto apply_inception = [&](const Address& at) {
    if (inceptions_ == nullptr) {
      return;
    }
    const Inception* graft = inceptions_->at_root(at);
    if (graft == nullptr) {
      return;
    }
    if (const auto* pin = std::get_if<SeedPin>(&graft->what)) {
      key = pin->pinned_key;
    }
    // KindSwap/AuthoredRef: stubbed — recorded in the store, not yet
    // honored by materialization.
  };

  if (!address.is_root()) {
    // Materialize (via cache) the parent chain, then derive this entry.
    parent = get(address.parent());
    if (parent == nullptr) {
      return nullptr;
    }
    const Step& step = address.last();
    const auto axis = parent->axis(step.axis);
    if (!axis.has_value()) {
      return nullptr;
    }
    if (!axis->at(step.at).has_value()) {
      return nullptr;  // unoccupied cell
    }
    key = axis->entry_key(step.at);
    kind = axis->desc().child_kind;
  }
  apply_inception(address);

  const Generator* generator = registry_.find(kind);
  if (generator == nullptr) {
    return nullptr;
  }
  Node probe(address, key, kind, {}, {});  // key holder for params_key()
  NodeSpec spec = (*generator)(probe.params_key(), parent.get());
  return std::make_shared<Node>(address, key, kind, std::move(spec.params),
                                std::move(spec.axes));
}

void InfinityTree::set_inceptions(std::shared_ptr<const InceptionStore> store) {
  // Invalidate everything under both stores' graft roots.
  const auto invalidate_for = [&](const std::shared_ptr<const InceptionStore>& s) {
    if (s == nullptr) {
      return;
    }
    for (const Inception& entry : s->entries()) {
      invalidate_prefix(entry.root);
    }
  };
  invalidate_for(inceptions_);
  invalidate_for(store);
  inceptions_ = std::move(store);
}

void InfinityTree::invalidate_prefix(const Address& prefix) {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (prefix.is_prefix_of(it->first)) {
      lru_.erase(it->second.lru_it);
      it = cache_.erase(it);
    } else {
      ++it;
    }
  }
}

void InfinityTree::touch(const Address& address) {
  auto it = cache_.find(address);
  if (it != cache_.end()) {
    lru_.erase(it->second.lru_it);
    lru_.push_front(address);
    it->second.lru_it = lru_.begin();
  }
}

void InfinityTree::clear_cache() {
  cache_.clear();
  lru_.clear();
}

}  // namespace inf::core::tree
