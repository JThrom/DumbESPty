#!/usr/bin/env bash
# Build, run, and generate a coverage report for the DumbESPty host unit tests.
#
# These are NATIVE (host gcc/g++) tests of the firmware's hardware-independent
# logic; they do NOT require ESP-IDF or hardware. See README.md for scope.
#
# Usage:
#   test/unit/run.sh             # configure, build, test, coverage report
#   test/unit/run.sh --no-cov    # build + test only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/unit"

WANT_COV=1
[[ "${1:-}" == "--no-cov" ]] && WANT_COV=0

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug
cmake --build "${BUILD_DIR}" -j"$(nproc)"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

if [[ "${WANT_COV}" == "1" ]]; then
  cmake --build "${BUILD_DIR}" --target coverage
  echo
  echo "Coverage report:"
  echo "  HTML:      ${BUILD_DIR}/coverage/index.html"
  echo "  Cobertura: ${BUILD_DIR}/coverage/coverage.xml"
  echo "  Text:      ${BUILD_DIR}/coverage/coverage.txt"
fi
