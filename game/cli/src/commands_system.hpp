#pragma once

#include <cstdint>

#include "core/seed.hpp"

namespace inf::cli {

int cmd_dump_system(const core::Seed128& seed, std::int64_t start_ns, std::int64_t span_ns,
                    int steps);
int cmd_hash_system();

}  // namespace inf::cli
