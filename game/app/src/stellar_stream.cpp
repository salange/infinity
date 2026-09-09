#include "stellar_stream.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

#include "core/time/world_clock.hpp"
#include "gen/universe.hpp"

namespace inf::app {
namespace {
using sim::Vec3;
using Clock = inf::core::MonotonicClock;
Vec3 vector(const gen::Dir3& p) {
  return {p.x.to_double(), p.y.to_double(), p.z.to_double()};
}
constexpr double kParsecLy = 3.2615638;
constexpr double kLimit = 8.3;
struct CachedCell {
  std::vector<gen::GalaxyOctree::StarSummary> stars;
  std::uint64_t used{0};
};
struct CatalogCache {
  using Key = std::tuple<int, int, std::int64_t, std::int64_t, std::int64_t>;
  std::map<Key, CachedCell> cells;
  std::map<Key, std::uint32_t> counts;
  std::size_t bytes{0};
  std::uint64_t epoch{0};
  void trim() {
    constexpr std::size_t budget = 64 * 1024 * 1024;
    for (auto it = cells.begin(); it != cells.end() && bytes > budget;) {
      if (it->second.used < epoch) {
        bytes -= it->second.stars.capacity() *
                 sizeof(gen::GalaxyOctree::StarSummary);
        it = cells.erase(it);
      } else
        ++it;
    }
    // Current corridor data is also evictable after its stars were emitted.
    // The cache is an optimization; metadata and transient arrays are bounded.
    while (bytes > budget && !cells.empty()) {
      const auto it = cells.begin();
      bytes -=
          it->second.stars.capacity() * sizeof(gen::GalaxyOctree::StarSummary);
      cells.erase(it);
    }
    if (counts.size() > 100000) counts.clear();
  }
};
}  // namespace
static StellarCatalog build_catalog(const gen::GalaxyOctree& galaxy, Vec3 eye,
                                    Vec3 velocity, CatalogCache& cache,
                                    const std::function<bool()>& cancelled,
                                    double magnitude_limit) {
  const auto begin = Clock::now();
  ++cache.epoch;
  cache.trim();
  StellarCatalog result;
  result.origin = eye;
  const double root = galaxy.cell_size_m(0);
  const Vec3 predicted = eye + velocity * 0.5;
  // One second of trajectory plus an all-direction guard. The visible set is
  // selected by actual distance in the shader, not by time since upload.
  const double guard = sim::length(velocity) * 0.75;
  constexpr float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1},
                                   {-1, -1}, {1, 1},  {-1, 1}};
  for (int band = 0; band < 24; ++band) {
    const double lower = -12.0 + band;
    const double upper = lower + 1.0;
    const double visibility = kParsecLy *
                              std::pow(10.0, (kLimit - lower + 5.0) / 5.0) *
                              gen::kLightYearM;
    int level = 0;
    while (level < gen::GalaxyOctree::kMaxLevel &&
           galaxy.cell_size_m(level) > visibility * 0.5)
      ++level;
    const double size = galaxy.cell_size_m(level);
    const auto cells = std::int64_t{1} << level;
    const auto grid = [&](double p) {
      return static_cast<std::int64_t>(std::floor((p + 0.5 * root) / size));
    };
    const double actual_visibility =
        visibility * std::pow(10.0, (magnitude_limit - kLimit) / 5.0);
    const double reach = actual_visibility + guard;
    const std::int64_t lo[3] = {
        std::max(std::int64_t{0}, grid(predicted.x - reach)),
        std::max(std::int64_t{0}, grid(predicted.y - reach)),
        std::max(std::int64_t{0}, grid(predicted.z - reach))};
    const std::int64_t hi[3] = {std::min(cells - 1, grid(predicted.x + reach)),
                                std::min(cells - 1, grid(predicted.y + reach)),
                                std::min(cells - 1, grid(predicted.z + reach))};
    std::function<void(const gen::GalaxyOctree::CellId&)> visit;
    visit = [&](const gen::GalaxyOctree::CellId& cell) {
      if (cancelled()) return;
      const double cell_size = galaxy.cell_size_m(cell.level);
      const Vec3 center = vector(galaxy.cell_center_m(cell));
      const Vec3 delta = center - eye;
      const double speed2 = sim::dot(velocity, velocity);
      const double along =
          speed2 > 0
              ? std::clamp(sim::dot(delta, velocity) / speed2, -.25, 1.25)
              : 0;
      if (sim::length(delta - velocity * along) >
          actual_visibility + cell_size * .866026)
        return;
      const auto key =
          CatalogCache::Key{band, cell.level, cell.x, cell.y, cell.z};
      auto number = cache.counts.find(key);
      if (number == cache.counts.end())
        number =
            cache.counts
                .emplace(key, galaxy.luminous_count(cell, det::Real(upper)))
                .first;
      // Split by intrinsic population, never by the eye position. A star's
      // owning cell therefore cannot change when the observer moves.
      if (number->second > 2048 && cell.level < gen::GalaxyOctree::kMaxLevel) {
        for (int c = 0; c < 8; ++c)
          visit({cell.x * 2 + (c & 1), cell.y * 2 + ((c >> 1) & 1),
                 cell.z * 2 + ((c >> 2) & 1), cell.level + 1});
        return;
      }
      auto found = cache.cells.find(key);
      if (found == cache.cells.end()) {
        CachedCell entry;
        const auto count = number->second;
        for (std::uint32_t index = 0; index < count; ++index) {
          if ((index & 255U) == 0 && cancelled()) return;
          const auto star = galaxy.luminous_star(cell, det::Real(upper), index);
          if (star.abs_mag.to_double() > lower || band == 0)
            entry.stars.push_back(star);
        }
        cache.bytes +=
            entry.stars.capacity() * sizeof(gen::GalaxyOctree::StarSummary);
        found = cache.cells.emplace(key, std::move(entry)).first;
      }
      found->second.used = cache.epoch;
      for (const auto& star : found->second.stars) {
        const double magnitude = star.abs_mag.to_double();
        const Vec3 absolute = vector(star.position_m);
        const Vec3 from_eye = absolute - eye;
        const double path_t =
            speed2 > 0
                ? std::clamp(sim::dot(from_eye, velocity) / speed2, -0.25, 1.25)
                : 0;
        const double nearest = sim::length(from_eye - velocity * path_t);
        const double apparent =
            magnitude +
            5.0 * std::log10(std::max(
                      1e-8, nearest / (kParsecLy * gen::kLightYearM))) -
            5.0;
        if (apparent > magnitude_limit) continue;
        const Vec3 relative = from_eye * (1.0 / gen::kLightYearM);
        const double temp = star.temperature_k.to_double();
        float tint[3];
        stellar_tint(temp, tint);
        const float packed =
            static_cast<float>(static_cast<int>(tint[0] * 255) * 65536 +
                               static_cast<int>(tint[1] * 255) * 256 +
                               static_cast<int>(tint[2] * 255));
        // Flux at one game light-year; the shader computes true distance and
        // its continuous visibility window every frame.
        const float flux = static_cast<float>(
            0.5 * std::pow(10.0, -0.4 * (magnitude -
                                         5.0 * std::log10(kParsecLy) - 5.0)));
        for (const auto& corner : corners)
          result.vertices.insert(
              result.vertices.end(),
              {static_cast<float>(relative.x), static_cast<float>(relative.y),
               static_cast<float>(relative.z), corner[0], corner[1], flux,
               packed, 0.0f, 0.0f, 0.0f});
      }
      cache.trim();
    };
    for (auto z = lo[2]; z <= hi[2] && !cancelled(); ++z)
      for (auto y = lo[1]; y <= hi[1] && !cancelled(); ++y)
        for (auto x = lo[0]; x <= hi[0] && !cancelled(); ++x)
          visit({x, y, z, level});
    cache.trim();
  }
  result.build_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
  return result;
}
StellarCatalog build_stellar_catalog(const gen::GalaxyOctree& galaxy, Vec3 eye,
                                     Vec3 velocity,
                                     const std::atomic<bool>* cancelled,
                                     double magnitude_limit) {
  CatalogCache cache;
  return build_catalog(
      galaxy, eye, velocity, cache,
      [&] { return cancelled && cancelled->load(); }, magnitude_limit);
}
struct StellarStream::Impl {
  gen::GalaxyOctree galaxy;
  std::mutex mutex;
  std::condition_variable wake;
  std::atomic<bool> cancel{false};
  bool pending{false};
  std::atomic<unsigned> trajectory{0};
  CatalogCache cache;
  Vec3 eye{}, velocity{};
  double limit{8.3};
  Clock::time_point requested{Clock::now()};
  std::unique_ptr<StellarCatalog> ready;
  std::thread worker;
  Impl(const core::Seed128& seed, const gen::GalaxyParams& parameters)
      : galaxy(gen::home_galaxy_key(seed), parameters) {
    worker = std::thread([this] {
      for (;;) {
        Vec3 at, speed;
        unsigned generation = 0;
        double magnitude = 8.3;
        {
          std::unique_lock lock(mutex);
          wake.wait(lock, [this] { return pending || cancel.load(); });
          if (cancel.load()) return;
          at = eye;
          speed = velocity;
          magnitude = limit;
          pending = false;
          generation = trajectory.load();
        }
        auto catalog = std::make_unique<StellarCatalog>(build_catalog(
            galaxy, at, speed, cache,
            [&] { return cancel.load() || trajectory.load() != generation; },
            magnitude));
        if (cancel.load()) return;
        std::lock_guard lock(mutex);
        if (trajectory.load() == generation) ready = std::move(catalog);
      }
    });
  }
  ~Impl() {
    cancel.store(true);
    wake.notify_one();
    worker.join();
  }
};
StellarStream::StellarStream(const core::Seed128& seed,
                             const gen::GalaxyParams& galaxy)
    : impl_(std::make_unique<Impl>(seed, galaxy)) {}
StellarStream::~StellarStream() = default;
void StellarStream::request(Vec3 eye, Vec3 velocity, double magnitude_limit) {
  std::lock_guard lock(impl_->mutex);
  const double scale = std::max(1.0, sim::length(impl_->velocity));
  const auto now = Clock::now();
  const double dt =
      std::chrono::duration<double>(now - impl_->requested).count();
  const Vec3 expected = impl_->eye + impl_->velocity * dt;
  if (sim::length(velocity - impl_->velocity) > scale * .25 ||
      sim::length(eye - expected) >
          std::max(gen::kLightYearM * .01, scale * .1)) {
    ++impl_->trajectory;
    impl_->ready.reset();
  }
  impl_->requested = now;
  impl_->eye = eye;
  impl_->velocity = velocity;
  impl_->limit = magnitude_limit;
  impl_->pending = true;
  impl_->wake.notify_one();
}
std::unique_ptr<StellarCatalog> StellarStream::take_ready() {
  std::lock_guard lock(impl_->mutex);
  return std::move(impl_->ready);
}
}  // namespace inf::app
