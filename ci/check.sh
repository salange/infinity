#!/usr/bin/env bash
# Full local gauntlet: configure, build, lint gates, tests, smoke runs.
# Usage: ci/check.sh [build-dir] — default build/
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"
GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
  GENERATOR_ARGS=(-G Ninja)
fi

echo "=== configure ==="
cmake -B "$BUILD_DIR" "${GENERATOR_ARGS[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo

echo "=== build ==="
cmake --build "$BUILD_DIR"

echo "=== grep gates ==="
SRC_DIRS="engine/core engine/world engine/render game/gen game/sim game/app game/cli"
# No platform RNG in generation paths (tests are exempt).
if grep -rn --include='*.hpp' --include='*.cpp' -E '\bstd::(mt19937|minstd_rand|random_device|uniform_[a-z_]+_distribution)\b|\brand\(\)' \
    $SRC_DIRS; then
  echo "FORBIDDEN: platform RNG in source tree" >&2
  exit 1
fi
# No naked new/delete in headless modules.
if grep -rn --include='*.cpp' --include='*.hpp' -E '(^|[^_[:alnum:]])(new|delete)[[:space:]]+[A-Za-z_]' \
    engine/core engine/world game/gen game/sim game/cli | grep -v 'delete$\|= delete'; then
  echo "FORBIDDEN: naked new/delete in headless modules" >&2
  exit 1
fi
# No libm transcendentals in deterministic modules (use core/det/trig.hpp).
if grep -rn --include='*.hpp' --include='*.cpp' -E 'std::(sin|cos|tan|asin|acos|atan|atan2|exp|exp2|log|log2|log10|pow|fmod|hypot|cbrt)\b' \
    engine/core engine/world game/gen | grep -v 'core/det/trig\|src/trig.cpp'; then
  echo "FORBIDDEN: libm transcendental in deterministic module" >&2
  exit 1
fi
# No OS-clock reads outside the clock module (planetary-systems spec §5).
if grep -rn --include='*.hpp' --include='*.cpp' -E 'system_clock|steady_clock|glfwGetTime|clock_gettime|\btime\(nullptr\)' \
    $SRC_DIRS | grep -v 'core/time/' | grep -v 'world_clock.cpp'; then
  echo "FORBIDDEN: OS clock read outside core/time" >&2
  exit 1
fi
# Civilization is a post-terrain layer (T0020): no terrain-side generator
# may include or read anything civ. civil/v1 hooks in from above.
if grep -rn --include='*.hpp' --include='*.cpp' -E '#include "gen/(civ|civil|colony|settlements|sites|buildings|ecumenopolis)' \
    game/gen/src/terrain.cpp game/gen/src/macro.cpp game/gen/src/drainage.cpp \
    game/gen/src/features.cpp game/gen/src/caves.cpp game/gen/src/provinces.cpp \
    game/gen/src/climate.cpp game/gen/src/life.cpp game/gen/src/biome.cpp \
    game/gen/include/gen/terrain.hpp game/gen/include/gen/macro.hpp \
    game/gen/include/gen/drainage.hpp game/gen/include/gen/features.hpp \
    game/gen/include/gen/caves.hpp game/gen/include/gen/provinces.hpp \
    game/gen/include/gen/climate.hpp game/gen/include/gen/life.hpp \
    game/gen/include/gen/biome.hpp engine/; then
  echo "FORBIDDEN: terrain-side generator reads civilization" >&2
  exit 1
fi

# Engine self-containment: nothing in engine/ includes game headers.
if grep -rn --include='*.hpp' --include='*.cpp' -E '#include "(gen|sim)/' engine/; then
  echo "FORBIDDEN: engine includes game headers" >&2
  exit 1
fi

echo "=== clang-tidy ==="
if command -v run-clang-tidy >/dev/null 2>&1; then
  run-clang-tidy -p "$BUILD_DIR" -quiet \
    "$(pwd)/(engine|game)/(core|world|render|gen|sim|app|cli)/.*" >/dev/null
else
  echo "run-clang-tidy not found — skipping (install clang-tools)" >&2
fi

echo "=== tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "=== cli smoke ==="
CLI="$BUILD_DIR/game/cli/infinity-cli"
"$CLI" --version
test "$("$CLI" --seed 0xDEADBEEF)" = "000000000000000000000000deadbeef"
if "$CLI" --seed nothex 2>/dev/null; then
  echo "FAIL: invalid seed accepted" >&2
  exit 1
fi

echo "=== golden hashes ==="
"$CLI" hash-core | diff - game/tests/goldens/hash-core.txt \
  || { echo "FAIL: hash-core diverges from goldens" >&2; exit 1; }
"$CLI" hash-planet | diff - game/tests/goldens/hash-planet.txt \
  || { echo "FAIL: hash-planet diverges from goldens" >&2; exit 1; }
"$CLI" hash-density | diff - game/tests/goldens/hash-density.txt \
  || { echo "FAIL: hash-density diverges from goldens" >&2; exit 1; }
"$CLI" hash-system | diff - game/tests/goldens/hash-system.txt \
  || { echo "FAIL: hash-system diverges from goldens" >&2; exit 1; }
"$CLI" hash-edits | diff - game/tests/goldens/hash-edits.txt \
  || { echo "FAIL: hash-edits diverges from goldens" >&2; exit 1; }
"$CLI" hash-civ | diff - game/tests/goldens/hash-civ.txt \
  || { echo "FAIL: hash-civ diverges from goldens" >&2; exit 1; }

echo "=== payload determinism ==="
"$CLI" dump-planet --seed 7 --type EarthLike > /tmp/infinity-dump-a.json
"$CLI" dump-planet --seed 7 --type EarthLike > /tmp/infinity-dump-b.json
diff /tmp/infinity-dump-a.json /tmp/infinity-dump-b.json \
  || { echo "FAIL: dump-planet not reproducible" >&2; exit 1; }
rm -f /tmp/infinity-dump-a.json /tmp/infinity-dump-b.json

echo "=== headless invariant ==="
if command -v ldd >/dev/null 2>&1; then
  if ldd "$CLI" | grep -Ei 'glfw|wgpu|vulkan|wayland|X11'; then
    echo "FAIL: headless cli links a window/GPU library" >&2
    exit 1
  fi
elif command -v otool >/dev/null 2>&1; then
  if otool -L "$CLI" | grep -Ei 'glfw|wgpu|Metal|Cocoa'; then
    echo "FAIL: headless cli links a window/GPU library" >&2
    exit 1
  fi
fi

echo "=== app smoke (60 frames, needs display) ==="
if [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] && [ -z "${INFINITY_SKIP_APP_SMOKE:-}" ]; then
  "$BUILD_DIR/game/app/infinity" --frames 60 --windowed
else
  echo "no display (or skipped) — app smoke not run"
fi

echo "ALL CHECKS PASSED"
