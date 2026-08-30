#!/usr/bin/env bash
# M8 cross-platform determinism check (T0010, prototype-v0 exit
# criterion 1): build the HEADLESS tools and emit the full golden suite
# for this platform. Run on every target platform, then diff the reports —
# they must be byte-identical:
#
#   ./ci/platform-report.sh                 # writes report-<os>-<arch>.txt
#   diff report-linux-x86_64.txt report-darwin-arm64.txt   # must be empty
#
# Any mismatch: bisect the differing suite section, find the det::real
# site (greppable by design), migrate it to fixed64, log in T0010.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build-headless}"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"
REPORT="report-${OS}-${ARCH}.txt"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DINFINITY_BUILD_APP=OFF
cmake --build "$BUILD_DIR" --target infinity-cli --parallel

"$BUILD_DIR/game/cli/infinity-cli" goldens > "$REPORT"
echo "wrote $REPORT ($(wc -l < "$REPORT") lines) — diff against the other platforms"
