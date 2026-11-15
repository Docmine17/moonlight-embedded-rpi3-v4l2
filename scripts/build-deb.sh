#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="moonlight-cross-trixie-arm64:latest"
DOCKERFILE="${PROJECT_ROOT}/docker/Dockerfile.cross-trixie-arm64"
BUILD_DIR="build-trixie-arm64"
REBUILD_IMAGE=0
CLEAN_BUILD=0

for arg in "$@"; do
  case "$arg" in
    --rebuild-image)
      REBUILD_IMAGE=1
      ;;
    --clean)
      CLEAN_BUILD=1
      ;;
    -h|--help)
      echo "Usage: $0 [--rebuild-image] [--clean]"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

# Prefer podman (rootless on Arch Linux maps container root directly to host UID)
if command -v podman >/dev/null 2>&1; then
  CONTAINER_ENGINE="podman"
  RUN_USER_ARGS=()
elif command -v docker >/dev/null 2>&1; then
  CONTAINER_ENGINE="docker"
  if docker info -f '{{.SecurityOptions}}' 2>/dev/null | grep -q rootless; then
    RUN_USER_ARGS=()
  else
    RUN_USER_ARGS=(--user "$(id -u):$(id -g)")
  fi
else
  echo "Error: Neither podman nor docker was found in PATH." >&2
  exit 1
fi

if [[ "${REBUILD_IMAGE}" -eq 1 ]] || ! "${CONTAINER_ENGINE}" image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
  echo "==> Building cross-compilation container image (${IMAGE_NAME}) via ${CONTAINER_ENGINE}..."
  "${CONTAINER_ENGINE}" build -t "${IMAGE_NAME}" -f "${DOCKERFILE}" "${PROJECT_ROOT}"
fi

if [[ "${CLEAN_BUILD}" -eq 1 ]]; then
  echo "==> Cleaning ${BUILD_DIR}..."
  rm -rf "${PROJECT_ROOT}/${BUILD_DIR}"
fi

mkdir -p "${PROJECT_ROOT}/${BUILD_DIR}"

echo "==> Cross-compiling (Release, Cortex-A53 + LTO) and packaging .deb (arm64 / Debian Trixie)..."
"${CONTAINER_ENGINE}" run --rm \
  "${RUN_USER_ARGS[@]}" \
  -v "${PROJECT_ROOT}:/workspace" \
  -w /workspace \
  "${IMAGE_NAME}" \
  bash -c "
    set -euo pipefail
    if [[ ! -f ${BUILD_DIR}/build.ninja ]]; then
      cmake -B ${BUILD_DIR} -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_TOOLCHAIN_FILE=/workspace/cmake/Toolchain-aarch64-linux-gnu.cmake
    fi
    cmake --build ${BUILD_DIR} --parallel
    if grep -q -- '-DLC_DEBUG' ${BUILD_DIR}/build.ninja; then
      echo 'Error: -DLC_DEBUG found in build.ninja; Release build expected.' >&2
      exit 1
    fi
    rm -f ${BUILD_DIR}/*.deb
    (cd ${BUILD_DIR} && cpack -G DEB)
  "

DEB_FILE="$(ls -1 "${PROJECT_ROOT}/${BUILD_DIR}"/*.deb | head -n 1)"
echo "==> Package ready: ${DEB_FILE}"
dpkg-deb -I "${DEB_FILE}" 2>/dev/null || "${CONTAINER_ENGINE}" run --rm -v "${PROJECT_ROOT}:/workspace" -w /workspace "${IMAGE_NAME}" dpkg-deb -I "${BUILD_DIR}/$(basename "${DEB_FILE}")"
