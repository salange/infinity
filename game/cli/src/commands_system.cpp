// Headless planetary-system inspection (T0012): params JSON + ephemeris
// table for eyeballing, plus the golden report.

#include "commands_system.hpp"

#include <cstdio>
#include <cstring>

#include "gen/system.hpp"
#include "gen/universe.hpp"

namespace inf::cli {

int cmd_dump_system(const core::Seed128& seed, std::int64_t start_ns, std::int64_t span_ns,
                    int steps) {
  const auto tree = gen::make_tree(seed);
  const auto node = tree->get(gen::default_system_address());
  const gen::StarSystemParams system = gen::generate_system(node->key());
  std::printf("{\n\"system\": %s,\n\"ephemeris\": %s}\n",
              gen::system_to_json(system).c_str(),
              gen::ephemeris_table_json(system, core::WorldTime::from_ns(start_ns),
                                        steps > 1 ? span_ns / (steps - 1) : span_ns, steps)
                  .c_str());
  return 0;
}

int cmd_hash_system() {
  std::fputs(gen::hash_system_report().c_str(), stdout);
  return 0;
}

}  // namespace inf::cli
