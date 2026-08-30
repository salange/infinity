#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

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
      "  dump-system --seed <hex128> [--start-ns N] [--span-ns N] [--steps N]\n"
      "                       print system params + ephemeris table as JSON\n"
      "  dump-planet --seed <hex128> [--type <T>]\n"
      "                       print planet params + province table as JSON\n"
      "  province-map --seed <hex128> [--type <T>] [--out <prefix>]\n"
      "                       write equirect province/relief PNGs\n"
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

  if (std::strcmp(argv[1], "dump-planet") == 0 || std::strcmp(argv[1], "province-map") == 0) {
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
    return inf::cli::cmd_province_map(*seed, type_text, out_prefix);
  }

  return print_usage();
}
