#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "core/seed.hpp"
#include "core/version.hpp"

namespace {

int print_usage() {
  std::printf(
      "usage: infinity-cli <command> [args]\n"
      "  --version            print version\n"
      "  --seed <hex128>      parse a universe seed, print canonical form\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return print_usage();
  }

  if (std::strcmp(argv[1], "--version") == 0) {
    std::printf("infinity-cli %s (%s)\n", inf::core::kVersion, inf::core::kGitHash);
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

  return print_usage();
}
