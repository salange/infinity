#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "commands_civ.hpp"
#include "commands_planet.hpp"
#include "commands_system.hpp"
#include "gen/golden.hpp"
#include "core/seed.hpp"
#include "gen/version.hpp"

namespace {

int print_usage() {
  std::printf(
      "usage: infinity-cli <command> [args]\n"
      "  --version            print version\n"
      "  --seed <hex128>      parse a universe seed, print canonical form\n"
      "  hash-core            print the deterministic-core golden report\n"
      "  hash-planet          print the planet/province golden report\n"
      "  hash-density         print the density-grid golden report\n"
      "  hash-system          print the planetary-system golden report\n"
      "  hash-edits           print the effective-density (player diff) golden report\n"
      "  hash-civ             print the civilization golden report\n"
      "  goldens              print the FULL golden suite (all reports, one\n"
      "                       machine-readable document — diff across platforms)\n"
      "  dump-system --seed <hex128> [--start-ns N] [--span-ns N] [--steps N]\n"
      "                       print system params + ephemeris table as JSON\n"
      "  dump-planet --seed <hex128> [--type <T>]\n"
      "                       print planet params + province table as JSON\n"
      "  province-map --seed <hex128> [--type <T>] [--out <prefix>]\n"
      "                       write equirect province/relief PNGs\n"
      "  terrain-map --seed <hex128> [--type <T>] [--out <prefix>]\n"
      "                       write an equirect land/ocean elevation PNG\n"
      "  surface-map --seed <hex128> [--type <T>] [--out <prefix>]\n"
      "                       write equirect climate/biome/material/life PNGs\n"
      "  find-land [--seed <hex128>] [--slot N] [--moon M]\n"
      "                       system bodies with life + land spots as script lines\n"
      "  probe-column --at x y z [--seed] [--slot N] [--moon M]\n"
      "                       analytic ground vs density crossings vs streamed meshes\n"
      "  life-stats [--seeds N]\n"
      "                       habitability / life stage histogram across seeds\n"
      "  tile-dump [--out <dir>] [--size N]\n"
      "                       write every procedural surface tile as PNGs\n"
      "  macro-stats [--seeds N]\n"
      "                       land-fraction + pattern report across seeds\n"
      "  civ enclaves [--seed <hex128>]\n"
      "                       human enclaves + dead gates across the home cluster\n"
      "  civ state [--seed <hex128>] [--system x y z L] [--time <+yr|YYYY-MM-DD>]\n"
      "                       owner + per-body civilization state of a system\n"
      "  civ site [--seed] [--system x y z L] [--slot N [--moon M]] [--tier T | --site I]\n"
      "           [--time] [--out f.png]\n"
      "                       one settlement: lots as a top-down PNG + app capture lines\n"
      "  civ map [--seed] [--system x y z L] [--slot N [--moon M]] [--time] [--out f.png]\n"
      "                       equirect settlement-plan PNG of a body (default: the\n"
      "                       highest-level settled body of the system)\n"
      "  civ census [--seed <hex128>] [--samples N] [--time <+yr|YYYY-MM-DD>] [--level N]\n"
      "                       pacing census over the human sphere\n"
      "  civ races [--seed <hex128>] [--at x y z | --all]\n"
      "                       races whose reach covers a point (galactocentric ly;\n"
      "                       default: the home system)\n"
      "  (types: EarthLike|Barren|Desert|Ice)\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return print_usage();
  }

  if (std::strcmp(argv[1], "--version") == 0) {
    std::printf("infinity-cli %s (%s)\n", inf::gen::kVersion, inf::gen::kGitHash);
    return 0;
  }

  if (std::strcmp(argv[1], "--seed") == 0) {
    if (argc < 3) {
      std::fprintf(stderr, "--seed needs a value (up to 32 hex digits)\n");
      return 2;
    }
    const std::optional<inf::core::Seed128> seed = inf::core::parse_seed(argv[2]);
    if (!seed.has_value()) {
      std::fprintf(stderr, "invalid seed: %s\n", argv[2]);
      return 1;
    }
    std::printf("%s\n", inf::core::to_hex(*seed).c_str());
    return 0;
  }

  if (std::strcmp(argv[1], "hash-core") == 0) {
    std::fputs(inf::gen::hash_core_report().c_str(), stdout);
    return 0;
  }

  if (std::strcmp(argv[1], "hash-planet") == 0) {
    return inf::cli::cmd_hash_planet();
  }

  if (std::strcmp(argv[1], "hash-density") == 0) {
    std::fputs(inf::gen::hash_density_report().c_str(), stdout);
    return 0;
  }

  if (std::strcmp(argv[1], "hash-edits") == 0) {
    std::fputs(inf::gen::hash_edits_report().c_str(), stdout);
    return 0;
  }

  if (std::strcmp(argv[1], "hash-civ") == 0) {
    return inf::cli::cmd_hash_civ();
  }

  if (std::strcmp(argv[1], "civ") == 0) {
    if (argc < 3) {
      std::fprintf(stderr, "civ needs a subcommand: races\n");
      return 2;
    }
    const char* seed_text = "83";
    const char* time_text = nullptr;
    double at[3] = {0.0, 0.0, 0.0};
    bool have_at = false;
    long long cell[4] = {0, 0, 0, 0};
    bool have_cell = false;
    int samples = 800;
    int min_level = 0;
    bool all = false;
    int slot = -1;
    int moon = -1;
    const char* out_path = "civ-map.png";
    const char* tier_text = nullptr;
    int site_index = -1;
    for (int i = 3; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
        seed_text = argv[++i];
      } else if (std::strcmp(argv[i], "--all") == 0) {
        all = true;
      } else if (std::strcmp(argv[i], "--at") == 0 && i + 3 < argc) {
        at[0] = std::atof(argv[++i]);
        at[1] = std::atof(argv[++i]);
        at[2] = std::atof(argv[++i]);
        have_at = true;
      } else if (std::strcmp(argv[i], "--system") == 0 && i + 4 < argc) {
        cell[0] = std::atoll(argv[++i]);
        cell[1] = std::atoll(argv[++i]);
        cell[2] = std::atoll(argv[++i]);
        cell[3] = std::atoll(argv[++i]);
        have_cell = true;
      } else if (std::strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
        time_text = argv[++i];
      } else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
        samples = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
        min_level = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
        slot = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--moon") == 0 && i + 1 < argc) {
        moon = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
        out_path = argv[++i];
      } else if (std::strcmp(argv[i], "--tier") == 0 && i + 1 < argc) {
        tier_text = argv[++i];
      } else if (std::strcmp(argv[i], "--site") == 0 && i + 1 < argc) {
        site_index = std::atoi(argv[++i]);
      }
    }
    const std::optional<inf::core::Seed128> seed = inf::core::parse_seed(seed_text);
    if (!seed.has_value()) {
      std::fprintf(stderr, "invalid seed: %s\n", seed_text);
      return 1;
    }
    if (std::strcmp(argv[2], "races") == 0) {
      return inf::cli::cmd_civ_races(*seed, have_at ? at : nullptr, all);
    }
    if (std::strcmp(argv[2], "enclaves") == 0) {
      return inf::cli::cmd_civ_enclaves(*seed);
    }
    if (std::strcmp(argv[2], "state") == 0) {
      return inf::cli::cmd_civ_state(*seed, have_cell ? cell : nullptr, time_text);
    }
    if (std::strcmp(argv[2], "census") == 0) {
      return inf::cli::cmd_civ_census(*seed, samples, time_text, min_level);
    }
    if (std::strcmp(argv[2], "site") == 0) {
      return inf::cli::cmd_civ_site(*seed, have_cell ? cell : nullptr, slot, moon, tier_text, site_index,
                                    time_text, out_path, have_at ? at : nullptr);
    }
    if (std::strcmp(argv[2], "map") == 0) {
      return inf::cli::cmd_civ_map(*seed, have_cell ? cell : nullptr, slot, moon, time_text, out_path);
    }
    std::fprintf(stderr, "unknown civ subcommand: %s\n", argv[2]);
    return 2;
  }

  // M8: the whole contract in one document — byte-identical across
  // platforms or the determinism promise is broken.
  if (std::strcmp(argv[1], "goldens") == 0) {
    std::printf("=== suite: core ===\n%s", inf::gen::hash_core_report().c_str());
    std::printf("=== suite: planet ===\n");
    if (const int rc = inf::cli::cmd_hash_planet(); rc != 0) {
      return rc;
    }
    std::printf("=== suite: density ===\n%s", inf::gen::hash_density_report().c_str());
    std::printf("=== suite: system ===\n");
    if (const int rc = inf::cli::cmd_hash_system(); rc != 0) {
      return rc;
    }
    std::printf("=== suite: edits ===\n%s", inf::gen::hash_edits_report().c_str());
    std::printf("=== suite: civ ===\n");
    return inf::cli::cmd_hash_civ();
  }

  if (std::strcmp(argv[1], "hash-system") == 0) {
    return inf::cli::cmd_hash_system();
  }

  if (std::strcmp(argv[1], "dump-system") == 0) {
    const char* seed_text = nullptr;
    long long start_ns = 0;
    long long span_ns = 3'155'760'000'000'000LL;  // one game year
    int steps = 12;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
        seed_text = argv[++i];
      } else if (std::strcmp(argv[i], "--start-ns") == 0 && i + 1 < argc) {
        start_ns = std::atoll(argv[++i]);
      } else if (std::strcmp(argv[i], "--span-ns") == 0 && i + 1 < argc) {
        span_ns = std::atoll(argv[++i]);
      } else if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
        steps = std::atoi(argv[++i]);
      }
    }
    if (seed_text == nullptr) {
      std::fprintf(stderr, "dump-system needs --seed <hex128>\n");
      return 2;
    }
    const std::optional<inf::core::Seed128> seed = inf::core::parse_seed(seed_text);
    if (!seed.has_value()) {
      std::fprintf(stderr, "invalid seed: %s\n", seed_text);
      return 1;
    }
    return inf::cli::cmd_dump_system(*seed, start_ns, span_ns, steps);
  }

  if (std::strcmp(argv[1], "terrain-stats") == 0) {
    const char* seed_text2 = "83";
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
        seed_text2 = argv[++i];
      }
    }
    const auto seed2 = inf::core::parse_seed(seed_text2);
    if (!seed2.has_value()) {
      std::fprintf(stderr, "invalid seed: %s\n", seed_text2);
      return 1;
    }
    return inf::cli::cmd_terrain_stats(*seed2);
  }

  if (std::strcmp(argv[1], "macro-stats") == 0) {
    int seeds = 100;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seeds") == 0 && i + 1 < argc) {
        seeds = std::atoi(argv[++i]);
      }
    }
    return inf::cli::cmd_macro_stats(seeds);
  }

  if (std::strcmp(argv[1], "find-land") == 0) {
    const char* seed_text = "83";
    int slot = -1;
    int moon = -1;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
        seed_text = argv[++i];
      } else if (std::strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
        slot = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--moon") == 0 && i + 1 < argc) {
        moon = std::atoi(argv[++i]);
      }
    }
    const std::optional<inf::core::Seed128> seed = inf::core::parse_seed(seed_text);
    if (!seed.has_value()) {
      std::fprintf(stderr, "invalid seed: %s\n", seed_text);
      return 1;
    }
    return inf::cli::cmd_find_land(*seed, slot, moon);
  }

  if (std::strcmp(argv[1], "probe-column") == 0) {
    const char* seed_text = "83";
    int slot = -1;
    int moon = -1;
    double at[3] = {0.0, 0.0, 0.0};
    bool have_at = false;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
        seed_text = argv[++i];
      } else if (std::strcmp(argv[i], "--slot") == 0 && i + 1 < argc) {
        slot = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--moon") == 0 && i + 1 < argc) {
        moon = std::atoi(argv[++i]);
      } else if (std::strcmp(argv[i], "--at") == 0 && i + 3 < argc) {
        at[0] = std::atof(argv[++i]);
        at[1] = std::atof(argv[++i]);
        at[2] = std::atof(argv[++i]);
        have_at = true;
      }
    }
    const std::optional<inf::core::Seed128> seed = inf::core::parse_seed(seed_text);
    if (!seed.has_value() || !have_at) {
      std::fprintf(stderr, "probe-column needs --at x y z (and a valid --seed)\n");
      return 1;
    }
    return inf::cli::cmd_probe_column(*seed, slot, moon, at[0], at[1], at[2]);
  }

  if (std::strcmp(argv[1], "life-stats") == 0) {
    int seeds = 200;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seeds") == 0 && i + 1 < argc) {
        seeds = std::atoi(argv[++i]);
      }
    }
    return inf::cli::cmd_life_stats(seeds);
  }

  if (std::strcmp(argv[1], "tile-dump") == 0) {
    const char* out_dir = ".";
    int size = 256;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
        out_dir = argv[++i];
      } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
        size = std::atoi(argv[++i]);
      }
    }
    return inf::cli::cmd_tile_dump(out_dir, size);
  }

  if (std::strcmp(argv[1], "dump-planet") == 0 || std::strcmp(argv[1], "province-map") == 0 ||
      std::strcmp(argv[1], "terrain-map") == 0 || std::strcmp(argv[1], "surface-map") == 0) {
    const char* seed_text = nullptr;
    const char* type_text = nullptr;
    const char* out_prefix = "planet";
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
        seed_text = argv[++i];
      } else if (std::strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
        type_text = argv[++i];
      } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
        out_prefix = argv[++i];
      }
    }
    if (seed_text == nullptr) {
      std::fprintf(stderr, "%s needs --seed <hex128>\n", argv[1]);
      return 2;
    }
    const std::optional<inf::core::Seed128> seed = inf::core::parse_seed(seed_text);
    if (!seed.has_value()) {
      std::fprintf(stderr, "invalid seed: %s\n", seed_text);
      return 1;
    }
    if (std::strcmp(argv[1], "dump-planet") == 0) {
      return inf::cli::cmd_dump_planet(*seed, type_text);
    }
    if (std::strcmp(argv[1], "terrain-map") == 0) {
      return inf::cli::cmd_terrain_map(*seed, type_text, out_prefix);
    }
    if (std::strcmp(argv[1], "surface-map") == 0) {
      return inf::cli::cmd_surface_map(*seed, type_text, out_prefix);
    }
    return inf::cli::cmd_province_map(*seed, type_text, out_prefix);
  }

  return print_usage();
}
