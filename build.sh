#!/bin/bash

set -e

PROJECT_ROOT=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR="${PROJECT_ROOT}/build"
INSTALL_DIR="${PROJECT_ROOT}/install"

echo "========================================"
echo " RK3568 YOLOv8 Project Build"
echo " Native build on Debian 11 aarch64"
echo "========================================"

echo "[1/5] Project root: ${PROJECT_ROOT}"

echo "[2/5] Cleaning old build directory..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

echo "[3/5] Configuring CMake..."
cd "${BUILD_DIR}"

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}"

echo "[4/5] Building..."
make -j$(nproc)

echo "[5/5] Installing..."
make install

echo "========================================"
echo " Build finished."
echo " Output directory:"
echo "   ${INSTALL_DIR}"
echo ""
echo " Run examples:"
echo "   cd ${INSTALL_DIR}"
echo "   export LD_LIBRARY_PATH=\$PWD/lib:\$LD_LIBRARY_PATH"
echo "   ./edge_ai_pipeline ../model/yolov8.rknn"
echo "   ./web_viewer 127.0.0.1 1883 edge/detect 8080"
echo "========================================"
