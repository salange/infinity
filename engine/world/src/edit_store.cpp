#include "world/edit_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>

namespace inf::world {

namespace {

constexpr std::uint32_t kMagic = 0x494E4644;  // "INFD"
constexpr std::uint32_t kVersion = 1;

double sq(double v) { return v * v; }

}  // namespace

std::uint64_t CsgEditStore::append(const SphereEdit& edit) {
  const std::unique_lock<std::shared_mutex> lock(mutex_);
  SphereEdit stored = edit;
  stored.op_id = next_op_id_++;
  edits_.push_back(stored);
  return stored.op_id;
}

std::size_t CsgEditStore::size() const {
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  return edits_.size();
}

std::vector<SphereEdit> CsgEditStore::overlapping(const det::Fixed64 center[3],
                                                  det::Fixed64 radius) const {
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<SphereEdit> hits;
  const double cx = center[0].to_double();
  const double cy = center[1].to_double();
  const double cz = center[2].to_double();
  const double r = radius.to_double();
  for (const SphereEdit& edit : edits_) {
    const double dist_sq = sq(edit.center(0).to_double() - cx) +
                           sq(edit.center(1).to_double() - cy) +
                           sq(edit.center(2).to_double() - cz);
    const double reach = r + edit.radius().to_double();
    if (dist_sq <= reach * reach) {
      hits.push_back(edit);
    }
  }
  return hits;  // op_id order preserved (append-only list)
}

bool CsgEditStore::save(const std::string& path) const {
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return false;
  }
  const std::uint32_t header[2] = {kMagic, kVersion};
  const auto count = static_cast<std::uint64_t>(edits_.size());
  file.write(reinterpret_cast<const char*>(header), sizeof(header));
  file.write(reinterpret_cast<const char*>(&count), sizeof(count));
  for (const SphereEdit& edit : edits_) {
    std::uint64_t record[6];
    record[0] = edit.op_id;
    std::memcpy(&record[1], edit.center_raw, sizeof(edit.center_raw));
    record[4] = static_cast<std::uint64_t>(edit.radius_raw);
    record[5] = (static_cast<std::uint64_t>(edit.material) << 1U) |
                (edit.subtract ? 1U : 0U);
    file.write(reinterpret_cast<const char*>(record), sizeof(record));
  }
  file.flush();
  return file.good();
}

bool CsgEditStore::load(const std::string& path) {
  const std::unique_lock<std::shared_mutex> lock(mutex_);
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::uint32_t header[2] = {0, 0};
  std::uint64_t count = 0;
  file.read(reinterpret_cast<char*>(header), sizeof(header));
  file.read(reinterpret_cast<char*>(&count), sizeof(count));
  bool ok = file.good() && header[0] == kMagic && header[1] == kVersion;
  edits_.clear();
  next_op_id_ = 1;
  for (std::uint64_t i = 0; ok && i < count; ++i) {
    std::uint64_t record[6];
    file.read(reinterpret_cast<char*>(record), sizeof(record));
    ok = file.good();
    if (!ok) {
      break;
    }
    SphereEdit edit;
    edit.op_id = record[0];
    std::memcpy(edit.center_raw, &record[1], sizeof(edit.center_raw));
    edit.radius_raw = static_cast<std::int64_t>(record[4]);
    edit.subtract = (record[5] & 1U) != 0;
    edit.material = static_cast<std::uint32_t>(record[5] >> 1U);
    edits_.push_back(edit);
    next_op_id_ = edit.op_id + 1;
  }
  if (!ok) {
    edits_.clear();
    next_op_id_ = 1;
  }
  return ok;
}

double apply_edits(double base_density, const std::vector<SphereEdit>& edits, double px,
                   double py, double pz) {
  double density = base_density;
  for (const SphereEdit& edit : edits) {
    const double dx = px - edit.center(0).to_double();
    const double dy = py - edit.center(1).to_double();
    const double dz = pz - edit.center(2).to_double();
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const double r = edit.radius().to_double();
    if (edit.subtract) {
      density = std::min(density, dist - r);
    } else {
      density = std::max(density, r - dist);
    }
  }
  return density;
}

}  // namespace inf::world
