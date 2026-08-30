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
# No platform RNG in generation paths (tests are exempt).
if grep -rn --include='*.hpp' --include='*.cpp' -E '\bstd::(mt19937|minstd_rand|random_device|uniform_[a-z_]+_distribution)\b|\brand\(\)' \
    core gen world sim render app cli; then
  echo "FORBIDDEN: platform RNG in source tree" >&2
  exit 1
fi
# No naked new/delete (clang-tidy owns this too; grep is the cheap backstop).
if grep -rn --include='*.cpp' --include='*.hpp' -E '(^|[^_[:alnum:]])(new|delete)[[:space:]]+[A-Za-z_]' \
    core gen world sim cli | grep -v 'delete$\|= delete'; then
  echo "FORBIDDEN: naked new/delete in headless modules" >&2
  exit 1
fi

# No libm transcendentals in deterministic modules (DECISIONS 2026-08-30):
# platform libms differ bit-for-bit. sqrt/floor/fabs/ceil are IEEE-exact and
# allowed; anything else needs our own deterministic implementation.
if grep -rn --include='*.hpp' --include='*.cpp' -E 'std::(sin|cos|tan|asin|acos|atan|atan2|exp|exp2|log|log2|log10|pow|fmod|hypot|cbrt)\b' \
    core gen world; then
  echo "FORBIDDEN: libm transcendental in deterministic module" >&2
  exit 1
fi

echo "=== clang-tidy ==="
if command -v run-clang-tidy >/dev/null 2>&1; then
  run-clang-tidy -p "$BUILD_DIR" -quiet \
    "$(pwd)/(core|gen|world|sim|render|app|cli)/.*" >/dev/null
else
  echo "run-clang-tidy not found — skipping (install clang-tools)" >&2
fi

echo "=== tests ==="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "=== cli smoke ==="
CLI="$BUILD_DIR/cli/infinity-cli"
"$CLI" --version
test "$("$CLI" --seed 0xDEADBEEF)" = "000000000000000000000000deadbeef"
if "$CLI" --seed nothex 2>/dev/null; then
  echo "FAIL: invalid seed accepted" >&2
  exit 1
fi

echo "=== golden hashes ==="
"$CLI" hash-core | diff - tests/goldens/hash-core.txt \
  || { echo "FAIL: hash-core diverges from goldens" >&2; exit 1; }

echo "=== headless invariant ==="
# cli must not link window/GPU libraries.
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
  "$BUILD_DIR/app/infinity" --frames 60
else
  echo "no display (or skipped) — app smoke not run"
fi

echo "ALL CHECKS PASSED"
