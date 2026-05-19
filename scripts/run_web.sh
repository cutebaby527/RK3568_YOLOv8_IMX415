#!/bin/bash
set -e

PROJECT_ROOT=$(cd "$(dirname "$0")/.." && pwd)
INSTALL_DIR="${PROJECT_ROOT}/install"
SHM_FILE="/dev/shm/edge_ai_frame"

cd "${INSTALL_DIR}"

export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${LD_LIBRARY_PATH}"

echo "[run_web] waiting for shared memory: ${SHM_FILE}"

for i in $(seq 1 30); do
    if [ -e "${SHM_FILE}" ]; then
        echo "[run_web] shared memory ready: ${SHM_FILE}"
        exec ./web_viewer \
          127.0.0.1 \
          1883 \
          edge/detect \
          8080 \
          edge/person/alarm \
          edge/person/metrics \
          ../config/config.json
    fi

    echo "[run_web] shared memory not ready, retry ${i}/30"
    sleep 1
done

echo "[run_web] timeout waiting for shared memory: ${SHM_FILE}" >&2
exit 1
