#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
INSTALL_DIR="${PROJECT_ROOT}/install"

cd "${INSTALL_DIR}"

export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

exec ./web_viewer \
  127.0.0.1 \
  1883 \
  edge/detect \
  8080 \
  edge/person/alarm \
  edge/person/metrics \
  ../config/config.json
