#!/usr/bin/env bash
# script for hard reset of project
# create fresh build folder ready for new compilation
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
rm -rf build result
cmake -S . -B build > /dev/null
