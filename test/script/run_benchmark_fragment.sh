#!/usr/bin/env bash
set -euo pipefail
ANIMATIONS=(mandelbrot julia voronoi waves tunnel ripple galaxy fire reaction cellular earth scene3d city ocean matrix)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$ROOT/test/script/benchmark_pipeline.py" --suite fragment "$@"
