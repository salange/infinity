#include "world/chunk_manager.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "core/golden.hpp"
#include "gen/cubesphere.hpp"

namespace inf::world {

namespace {

using det::Real;

// Strict ordering for addresses (stable iteration everywhere).
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

}  // namespace

struct ChunkManager::Impl {
  gen::PlanetParams planet;
  gen::TerrainField field;
  ChunkManagerConfig config;

  // Cache and bookkeeping (main-thread side).
  std::map<core::ChunkAddr, std::shared_ptr<const ChunkData>, AddrLess> ready;
  std::map<core::ChunkAddr, std::uint64_t, AddrLess> last_wanted;  // frame stamp
  std::set<core::ChunkAddr, AddrLess> in_flight;
  std::uint64_t frame = 0;

  // Worker pool.
  std::vector<std::thread> workers;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<core::ChunkAddr> queue;
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
      core::ChunkAddr addr;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [this] { return stopping || !queue.empty(); });
        if (stopping) {
          return;
        }
        addr = queue.front();
        queue.pop_front();
      }
      auto data = std::make_shared<ChunkData>();
      data->addr = addr;
      const gen::ChunkGrid grid = gen::ChunkGrid::from_addr(addr, planet);
      const gen::PaddedDensity padded = gen::sample_chunk_density_padded(field, grid);
      data->density_hash = [&padded] {
        // Inner (unpadded) slice — matches hash-density's artifact.
        core::GoldenHash hash;
        for (int gz = 0; gz <= static_cast<int>(gen::ChunkGrid::kVoxels); ++gz) {
          for (int gy = 0; gy <= static_cast<int>(gen::ChunkGrid::kVoxels); ++gy) {
            for (int gx = 0; gx <= static_cast<int>(gen::ChunkGrid::kVoxels); ++gx) {
              hash.feed(std::bit_cast<std::uint64_t>(padded.at(gx, gy, gz).to_double()));
            }
          }
        }
        return hash.value();
      }();
      data->mesh = gen::mesh_chunk(grid, padded);
      {
        const std::lock_guard<std::mutex> lock(done_mutex);
        done.push_back(std::move(data));
      }
    }
  }

  // ---- desired-set computation ------------------------------------------

  // Approximate center direction of a quad cell.
  gen::Dir3 cell_center_dir(std::uint8_t face, std::uint8_t lod, std::uint32_t i,
                            std::uint32_t j) const {
    const double cells = static_cast<double>(std::uint64_t{1} << lod);
    const double u = -1.0 + 2.0 * (static_cast<double>(i) + 0.5) / cells;
    const double v = -1.0 + 2.0 * (static_cast<double>(j) + 0.5) / cells;
    return gen::face_uv_to_dir(gen::FaceUV{face, Real(u), Real(v)});
  }

  void collect_leaves(std::uint8_t face, std::uint8_t lod, std::uint32_t i, std::uint32_t j,
                      const Vec3d& camera, double camera_radius,
                      std::vector<core::ChunkAddr>* out) const {
    const double radius = planet.radius_m.to_double();
    const double size = 2.0 * radius / static_cast<double>(std::uint64_t{1} << lod);
    const gen::Dir3 center = cell_center_dir(face, lod, i, j);
    const Vec3d center_pos{center.x.to_double() * radius, center.y.to_double() * radius,
                           center.z.to_double() * radius};
    const Vec3d delta{camera.x - center_pos.x, camera.y - center_pos.y,
                      camera.z - center_pos.z};
    // Conservative distance to the cell region (center distance minus its
    // bounding radius; lateral + shell extent).
    const double distance = std::max(1.0, length(delta) - size);

    const bool split = lod < config.max_lod && size / distance > config.split_factor;
    if (!split) {
      emit_column(face, lod, i, j, center, out);
      return;
    }
    for (std::uint32_t di = 0; di < 2; ++di) {
      for (std::uint32_t dj = 0; dj < 2; ++dj) {
        collect_leaves(face, static_cast<std::uint8_t>(lod + 1), i * 2 + di, j * 2 + dj, camera,
                       camera_radius, out);
      }
    }
    (void)camera_radius;
  }

  // Shells covering the local surface for a leaf column.
  void emit_column(std::uint8_t face, std::uint8_t lod, std::uint32_t i, std::uint32_t j,
                   const gen::Dir3& center, std::vector<core::ChunkAddr>* out) const {
    const double radius = planet.radius_m.to_double();
    const double thickness = 2.0 * radius / static_cast<double>(std::uint64_t{1} << lod);
    const double elevation = field.elevation_m(center).to_double();
    const int shell_mid = static_cast<int>(std::floor(elevation / thickness + 0.5));
    // One shell of margin: elevation varies within the column; coarse
    // chunks (large thickness) cover everything with shell 0 +- 1.
    for (int shell = shell_mid - 1; shell <= shell_mid + 1; ++shell) {
      if (shell < -32768 || shell > 32767) {
        continue;
      }
      out->push_back(core::ChunkAddr{face, lod, i, j, static_cast<std::int16_t>(shell)});
    }
  }

  std::vector<core::ChunkAddr> desired_set(const Vec3d& camera) const {
    std::vector<core::ChunkAddr> desired;
    const double camera_radius = length(camera);
    for (std::uint8_t face = 0; face < 6; ++face) {
      collect_leaves(face, 0, 0, 0, camera, camera_radius, &desired);
    }
    std::sort(desired.begin(), desired.end(), [](const auto& a, const auto& b) {
      return AddrLess{}(a, b);
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

  // 1. Desired set for this camera.
  const std::vector<core::ChunkAddr> desired =
      impl.desired_set(Vec3d{camera_x, camera_y, camera_z});
  for (const core::ChunkAddr& addr : desired) {
    impl.last_wanted[addr] = impl.frame;
  }

  // 2. Schedule missing chunks (sorted order — deterministic queue).
  {
    const std::lock_guard<std::mutex> lock(impl.queue_mutex);
    for (const core::ChunkAddr& addr : desired) {
      if (impl.ready.contains(addr) || impl.in_flight.contains(addr)) {
        continue;
      }
      impl.in_flight.insert(addr);
      impl.queue.push_back(addr);
    }
  }
  impl.queue_cv.notify_all();

  // 3. Collect finished chunks (bounded per update).
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

  // 4. Evict over budget: least-recently-wanted first, never currently
  // desired ones.
  if (impl.ready.size() > impl.config.resident_budget) {
    std::vector<std::pair<std::uint64_t, core::ChunkAddr>> candidates;
    for (const auto& [addr, data] : impl.ready) {
      const auto it = impl.last_wanted.find(addr);
      const std::uint64_t stamp = it == impl.last_wanted.end() ? 0 : it->second;
      if (stamp != impl.frame) {
        candidates.emplace_back(stamp, addr);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
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
  // Pure recomputation, independent of cache/scheduling state.
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
  // in_flight shrinks only in update(); done grows only in workers. All
  // scheduled work is finished exactly when the queue is empty and every
  // in-flight chunk's result sits in done.
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
