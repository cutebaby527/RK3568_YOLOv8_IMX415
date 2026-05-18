#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
INSTALL_DIR="${PROJECT_ROOT}/install"

cd "${INSTALL_DIR}"

export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

exec ./edge_ai_pipeline --config ../config/config.json
