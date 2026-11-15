#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build-trixie-arm64"

if [[ -f "${PROJECT_ROOT}/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "${PROJECT_ROOT}/.env.local"
fi

NO_BUILD=0
PI_TARGET="${PI_TARGET:-pi@10.42.0.92}"
BUILD_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build)
      NO_BUILD=1
      shift
      ;;
    --clean|--rebuild-image)
      BUILD_ARGS+=("$1")
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [user@pi-host] [--no-build] [--clean] [--rebuild-image]"
      echo ""
      echo "Default target: ${PI_TARGET} (override with PI_TARGET in .env.local or positional argument)."
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      exit 1
      ;;
    *)
      PI_TARGET="$1"
      shift
      ;;
  esac
done

if [[ "${NO_BUILD}" -eq 0 ]]; then
  "${SCRIPT_DIR}/build-deb.sh" "${BUILD_ARGS[@]}"
fi

DEB_FILE="$(ls -1 "${BUILD_DIR}"/*.deb 2>/dev/null | head -n 1 || true)"
if [[ -z "${DEB_FILE}" || ! -f "${DEB_FILE}" ]]; then
  echo "Error: No .deb package found in ${BUILD_DIR}. Run ./scripts/build-deb.sh first." >&2
  exit 1
fi

DEB_BASENAME="$(basename "${DEB_FILE}")"
REMOTE_TMP="/tmp/${DEB_BASENAME}"

SSH_CONTROL_DIR="${HOME}/.ssh/cm_socket"
mkdir -p -m 700 "${SSH_CONTROL_DIR}"
SSH_OPTS=(
  -F /dev/null
  -o "StrictHostKeyChecking=no"
  -o "ControlMaster=auto"
  -o "ControlPath=${SSH_CONTROL_DIR}/%r@%h:%p"
  -o "ControlPersist=10m"
)

echo "==> Copying ${DEB_BASENAME} to ${PI_TARGET}:${REMOTE_TMP}..."
scp "${SSH_OPTS[@]}" "${DEB_FILE}" "${PI_TARGET}:${REMOTE_TMP}"

echo "==> Installing ${DEB_BASENAME} on ${PI_TARGET}..."
ssh "${SSH_OPTS[@]}" "${PI_TARGET}" \
  "sudo dpkg -i '${REMOTE_TMP}' && rm -f '${REMOTE_TMP}' && moonlight --version && /sbin/getcap /usr/bin/moonlight"

echo "==> Successfully deployed and installed ${DEB_BASENAME} on ${PI_TARGET}!"
