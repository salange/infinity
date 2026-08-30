#include "world/chunk_manager.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/golden.hpp"
#include "gen/cubesphere.hpp"

namespace inf::world {

namespace {

using det::Real;

struct AddrLess {
  bool operator()(const core::ChunkAddr& a, const core::ChunkAddr& b) const {
    if (a.face != b.face) return a.face < b.face;
    if (a.lod != b.lod) return a.lod < b.lod;
    if (a.i != b.i) return a.i < b.i;
    if (a.j != b.j) return a.j < b.j;
    return a.shell < b.shell;
  }
};

struct Vec3d {
  double x, y, z;
};

double length(const Vec3d& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// A leaf column of the per-face quadtree (all shells of one (face,lod,i,j)).
struct Column {
  std::uint8_t face;
  std::uint8_t lod;
  std::uint32_t i;
  std::uint32_t j;
};

std::uint64_t pack_column(std::uint8_t face, std::uint8_t lod, std::uint32_t i,
                          std::uint32_t j) {
  return (static_cast<std::uint64_t>(face) << 60U) | (static_cast<std::uint64_t>(lod) << 52U) |
         (static_cast<std::uint64_t>(i) << 26U) | j;
}

}  // namespace

struct ChunkManager::Impl {
  gen::PlanetParams planet;
  gen::TerrainField field;
  ChunkManagerConfig config;

  struct Job {
    core::ChunkAddr addr;
    gen::TransitionMask mask;
  };

  // Main-thread bookkeeping.
  std::map<core::ChunkAddr, std::shared_ptr<const ChunkData>, AddrLess> ready;
  std::map<core::ChunkAddr, std::uint64_t, AddrLess> last_wanted;
  std::map<core::ChunkAddr, gen::TransitionMask, AddrLess> in_flight;
  std::uint64_t frame = 0;

  // Worker pool.
  std::vector<std::thread> workers;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<Job> queue;
  bool stopping = false;
  std::mutex done_mutex;
  std::vector<std::shared_ptr<const ChunkData>> done;

  Impl(const core::Key& body_key, const gen::PlanetParams& planet_params,
       const ChunkManagerConfig& cfg)
      : planet(planet_params), field(body_key, planet_params), config(cfg) {
    const std::uint32_t count = config.worker_count == 0 ? 1 : config.worker_count;
    workers.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      workers.emplace_back([this] { worker_main(); });
    }
  }

  ~Impl() {
    {
      const std::lock_guard<std::mutex> lock(queue_mutex);
      stopping = true;
    }
    queue_cv.notify_all();
    for (std::thread& worker : workers) {
      worker.join();
    }
  }

  void worker_main() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [this] { return stopping || !queue.empty(); });
        if (stopping) {
          return;
        }
        job = queue.front();
        queue.pop_front();
      }
      auto data = std::make_shared<ChunkData>();
      data->addr = job.addr;
      data->transitions = job.mask;
      const gen::ChunkGrid grid = gen::ChunkGrid::from_addr(job.addr, planet);
      const gen::PaddedDensity padded = gen::sample_chunk_density_padded(field, grid);
      core::GoldenHash hash;
      for (int gz = 0; gz <= static_cast<int>(gen::ChunkGrid::kVoxels); ++gz) {
        for (int gy = 0; gy <= static_cast<int>(gen::ChunkGrid::kVoxels); ++gy) {
          for (int gx = 0; gx <= static_cast<int>(gen::ChunkGrid::kVoxels); ++gx) {
            hash.feed(std::bit_cast<std::uint64_t>(padded.at(gx, gy, gz).to_double()));
          }
        }
      }
      data->density_hash = hash.value();
      data->mesh = gen::mesh_chunk(grid, padded, job.mask);
      {
        const std::lock_guard<std::mutex> lock(done_mutex);
        done.push_back(std::move(data));
      }
    }
  }

  // ---- desired-set computation ------------------------------------------

  gen::Dir3 cell_center_dir(std::uint8_t face, std::uint8_t lod, std::uint32_t i,
                            std::uint32_t j) const {
    const double cells = static_cast<double>(std::uint64_t{1} << lod);
    const double u = -1.0 + 2.0 * (static_cast<double>(i) + 0.5) / cells;
    const double v = -1.0 + 2.0 * (static_cast<double>(j) + 0.5) / cells;
    return gen::face_uv_to_dir(gen::FaceUV{face, Real(u), Real(v)});
  }

  void collect_columns(std::uint8_t face, std::uint8_t lod, std::uint32_t i, std::uint32_t j,
                       const Vec3d& camera, std::vector<Column>* out) const {
    const double radius = planet.radius_m.to_double();
    const double size = 2.0 * radius / static_cast<double>(std::uint64_t{1} << lod);
    const gen::Dir3 center = cell_center_dir(face, lod, i, j);
    const Vec3d center_pos{center.x.to_double() * radius, center.y.to_double() * radius,
                           center.z.to_double() * radius};
    const Vec3d delta{camera.x - center_pos.x, camera.y - center_pos.y,
                      camera.z - center_pos.z};
    const double distance = std::max(1.0, length(delta) - size);
    const bool split = lod < config.max_lod && size / distance > config.split_factor;
    if (!split) {
      out->push_back(Column{face, lod, i, j});
      return;
    }
    for (std::uint32_t di = 0; di < 2; ++di) {
      for (std::uint32_t dj = 0; dj < 2; ++dj) {
        collect_columns(face, static_cast<std::uint8_t>(lod + 1), i * 2 + di, j * 2 + dj,
                        camera, out);
      }
    }
  }

  // Column containing a direction at a given lod.
  static Column column_at(const gen::Dir3& dir, std::uint8_t lod) {
    const gen::FaceUV face_uv = gen::dir_to_face_uv(dir);
    const double cells = static_cast<double>(std::uint64_t{1} << lod);
    auto to_cell = [&](double coord) {
      const double f = (coord + 1.0) * 0.5 * cells;
      if (f < 0.0) return std::uint32_t{0};
      if (f >= cells) return static_cast<std::uint32_t>(cells - 1.0);
      return static_cast<std::uint32_t>(f);
    };
    return Column{face_uv.face, lod, to_cell(face_uv.u.to_double()),
                  to_cell(face_uv.v.to_double())};
  }

  // Probe direction just beyond one lateral boundary of a column.
  // side: 0 = u-, 1 = u+, 2 = v-, 3 = v+ (matches gen::TransitionFace bits).
  gen::Dir3 neighbor_probe(const Column& col, int side) const {
    const double cells = static_cast<double>(std::uint64_t{1} << col.lod);
    const double span = 2.0 / cells;
    const double u_lo = -1.0 + col.i * span;
    const double v_lo = -1.0 + col.j * span;
    double u = u_lo + span * 0.5;
    double v = v_lo + span * 0.5;
    const double eps = span * 0.1;
    if (side == 0) u = u_lo - eps;
    if (side == 1) u = u_lo + span + eps;
    if (side == 2) v = v_lo - eps;
    if (side == 3) v = v_lo + span + eps;
    return gen::face_uv_to_dir(gen::FaceUV{col.face, Real(u), Real(v)});
  }

  // Find the leaf column containing dir (walking down from max_lod).
  // Returns lod 0xFF if none found (cannot happen for a full partition).
  std::uint8_t leaf_lod_at(const std::unordered_set<std::uint64_t>& leaves,
                           const gen::Dir3& dir) const {
    for (int lod = config.max_lod; lod >= 0; --lod) {
      const Column col = column_at(dir, static_cast<std::uint8_t>(lod));
      if (leaves.contains(pack_column(col.face, col.lod, col.i, col.j))) {
        return static_cast<std::uint8_t>(lod);
      }
    }
    return 0xFF;
  }

  // Enforce max 1 lod level difference between lateral neighbors by
  // splitting coarser columns until stable (restricted quadtree).
  void balance(std::vector<Column>* columns) const {
    std::unordered_set<std::uint64_t> leaves;
    std::unordered_map<std::uint64_t, Column> by_key;
    for (const Column& col : *columns) {
      const std::uint64_t key = pack_column(col.face, col.lod, col.i, col.j);
      leaves.insert(key);
      by_key.emplace(key, col);
    }
    std::deque<Column> pending(columns->begin(), columns->end());
    int guard = 0;
    while (!pending.empty() && guard++ < 200000) {
      const Column col = pending.front();
      pending.pop_front();
      const std::uint64_t self_key = pack_column(col.face, col.lod, col.i, col.j);
      if (!leaves.contains(self_key)) {
        continue;  // was split away meanwhile
      }
      for (int side = 0; side < 4; ++side) {
        const gen::Dir3 probe = neighbor_probe(col, side);
        const std::uint8_t neighbor_lod = leaf_lod_at(leaves, probe);
        if (neighbor_lod == 0xFF || neighbor_lod + 1 >= col.lod) {
          continue;
        }
        // Neighbor too coarse: split it into its 4 children.
        const Column coarse = column_at(probe, neighbor_lod);
        const std::uint64_t coarse_key =
            pack_column(coarse.face, coarse.lod, coarse.i, coarse.j);
        leaves.erase(coarse_key);
        by_key.erase(coarse_key);
        for (std::uint32_t di = 0; di < 2; ++di) {
          for (std::uint32_t dj = 0; dj < 2; ++dj) {
            const Column child{coarse.face, static_cast<std::uint8_t>(coarse.lod + 1),
                               coarse.i * 2 + di, coarse.j * 2 + dj};
            const std::uint64_t child_key =
                pack_column(child.face, child.lod, child.i, child.j);
            leaves.insert(child_key);
            by_key.emplace(child_key, child);
            pending.push_back(child);
          }
        }
        pending.push_back(col);  // recheck this column against the refreshed leaf set
        break;
      }
    }
    columns->clear();
    columns->reserve(by_key.size());
    for (const auto& [key, col] : by_key) {
      columns->push_back(col);
    }
  }

  gen::TransitionMask column_mask(const std::unordered_set<std::uint64_t>& leaves,
                                  const Column& col) const {
    gen::TransitionMask mask = 0;
    for (int side = 0; side < 4; ++side) {
      const std::uint8_t neighbor_lod = leaf_lod_at(leaves, neighbor_probe(col, side));
      if (neighbor_lod != 0xFF && neighbor_lod + 1 == col.lod) {
        mask |= static_cast<gen::TransitionMask>(1U << side);
      }
    }
    return mask;
  }

  void emit_column(const Column& col, gen::TransitionMask mask,
                   std::vector<std::pair<core::ChunkAddr, gen::TransitionMask>>* out) const {
    const double radius = planet.radius_m.to_double();
    const double thickness = 2.0 * radius / static_cast<double>(std::uint64_t{1} << col.lod);
    const gen::Dir3 center = cell_center_dir(col.face, col.lod, col.i, col.j);
    const double elevation = field.elevation_m(center).to_double();
    const int shell_mid = static_cast<int>(std::floor(elevation / thickness));
    for (int shell = shell_mid - 1; shell <= shell_mid + 1; ++shell) {
      if (shell < -32768 || shell > 32767) {
        continue;
      }
      out->push_back({core::ChunkAddr{col.face, col.lod, col.i, col.j,
                                      static_cast<std::int16_t>(shell)},
                      mask});
    }
  }

  std::vector<std::pair<core::ChunkAddr, gen::TransitionMask>> desired_set(
      const Vec3d& camera) const {
    std::vector<Column> columns;
    for (std::uint8_t face = 0; face < 6; ++face) {
      collect_columns(face, 0, 0, 0, camera, &columns);
    }
    balance(&columns);
    std::unordered_set<std::uint64_t> leaves;
    leaves.reserve(columns.size() * 2);
    for (const Column& col : columns) {
      leaves.insert(pack_column(col.face, col.lod, col.i, col.j));
    }
    std::vector<std::pair<core::ChunkAddr, gen::TransitionMask>> desired;
    desired.reserve(columns.size() * 3);
    for (const Column& col : columns) {
      emit_column(col, column_mask(leaves, col), &desired);
    }
    std::sort(desired.begin(), desired.end(), [](const auto& a, const auto& b) {
      return AddrLess{}(a.first, b.first);
    });
    return desired;
  }
};

ChunkManager::ChunkManager(const core::Key& body_key, const gen::PlanetParams& planet,
                           const ChunkManagerConfig& config)
    : impl_(std::make_unique<Impl>(body_key, planet, config)) {}

ChunkManager::~ChunkManager() = default;

std::vector<ChunkEvent> ChunkManager::update(double camera_x, double camera_y,
                                             double camera_z) {
  Impl& impl = *impl_;
  ++impl.frame;
  std::vector<ChunkEvent> events;

  const auto desired = impl.desired_set(Vec3d{camera_x, camera_y, camera_z});
  for (const auto& [addr, mask] : desired) {
    impl.last_wanted[addr] = impl.frame;
  }

  // Schedule missing chunks and re-mesh chunks whose transition mask
  // changed (neighbor lod change). In-flight chunks are left to finish;
  // a stale mask is caught on a later update.
  {
    const std::lock_guard<std::mutex> lock(impl.queue_mutex);
    for (const auto& [addr, mask] : desired) {
      if (impl.in_flight.contains(addr)) {
        continue;
      }
      const auto it = impl.ready.find(addr);
      if (it != impl.ready.end() && it->second->transitions == mask) {
        continue;
      }
      impl.in_flight.emplace(addr, mask);
      impl.queue.push_back(Impl::Job{addr, mask});
    }
  }
  impl.queue_cv.notify_all();

  std::vector<std::shared_ptr<const ChunkData>> finished;
  {
    const std::lock_guard<std::mutex> lock(impl.done_mutex);
    const std::size_t take = std::min(impl.done.size(), impl.config.uploads_per_update);
    finished.assign(impl.done.begin(),
                    impl.done.begin() + static_cast<std::ptrdiff_t>(take));
    impl.done.erase(impl.done.begin(), impl.done.begin() + static_cast<std::ptrdiff_t>(take));
  }
  for (auto& data : finished) {
    impl.in_flight.erase(data->addr);
    impl.ready[data->addr] = data;
    events.push_back(ChunkEvent{ChunkEvent::Kind::Ready, data->addr, data});
  }

  if (impl.ready.size() > impl.config.resident_budget) {
    std::vector<std::pair<std::uint64_t, core::ChunkAddr>> candidates;
    for (const auto& [addr, data] : impl.ready) {
      const auto it = impl.last_wanted.find(addr);
      const std::uint64_t stamp = it == impl.last_wanted.end() ? 0 : it->second;
      if (stamp != impl.frame) {
        candidates.emplace_back(stamp, addr);
      }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
      if (a.first != b.first) return a.first < b.first;
      return AddrLess{}(a.second, b.second);
    });
    for (const auto& [stamp, addr] : candidates) {
      if (impl.ready.size() <= impl.config.resident_budget) {
        break;
      }
      impl.ready.erase(addr);
      impl.last_wanted.erase(addr);
      events.push_back(ChunkEvent{ChunkEvent::Kind::Evicted, addr, nullptr});
    }
  }
  return events;
}

std::vector<std::shared_ptr<const ChunkData>> ChunkManager::resident_chunks() const {
  std::vector<std::shared_ptr<const ChunkData>> chunks;
  chunks.reserve(impl_->ready.size());
  for (const auto& [addr, data] : impl_->ready) {
    chunks.push_back(data);
  }
  return chunks;
}

const gen::TerrainField& ChunkManager::field() const { return impl_->field; }

std::uint64_t ChunkManager::scene_hash(const std::vector<core::ChunkAddr>& addrs) const {
  std::vector<core::ChunkAddr> sorted = addrs;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return AddrLess{}(a, b); });
  core::GoldenHash hash;
  for (const core::ChunkAddr& addr : sorted) {
    const gen::ChunkGrid grid = gen::ChunkGrid::from_addr(addr, impl_->planet);
    hash.feed(gen::hash_chunk_density(impl_->field, grid));
  }
  return hash.value();
}

void ChunkManager::drain() {
  for (;;) {
    {
      const std::lock_guard<std::mutex> queue_lock(impl_->queue_mutex);
      const std::lock_guard<std::mutex> done_lock(impl_->done_mutex);
      if (impl_->queue.empty() && impl_->done.size() == impl_->in_flight.size()) {
        return;
      }
    }
    std::this_thread::yield();
  }
}

}  // namespace inf::world
