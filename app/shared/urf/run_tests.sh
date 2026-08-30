#!/usr/bin/env bash
# URF 编码器单测。在开发机上跑，不需要手机、不需要打印机。
set -euo pipefail
cd "$(dirname "$0")"
export URF_FIXTURE_DIR="$PWD/tests/fixtures"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build --parallel
ctest --test-dir build --output-on-failure
