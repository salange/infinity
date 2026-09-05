#pragma once

#include <cstdint>

#include "core/seed.hpp"

namespace inf::cli {

int cmd_dump_system(const core::Seed128& seed, std::int64_t start_ns, std::int64_t span_ns,
                    int steps);
int cmd_hash_system();

// T0019 capture aid: system slot bodies with life, plus gentle land spots.
int cmd_find_land(const core::Seed128& seed, int slot, int moon = -1);

int cmd_probe_column(const core::Seed128& seed, int slot, int moon, double x, double y, double z);

}  // namespace inf::cli
