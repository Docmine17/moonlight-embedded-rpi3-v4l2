#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ -f "${PROJECT_ROOT}/.env.local" ]]; then
  # shellcheck disable=SC1091
  source "${PROJECT_ROOT}/.env.local"
fi

PI_TARGET="${PI_TARGET:-pi@10.42.0.92}"
LOCAL_MODE=0
DRY_RUN=0
ENABLE_OC=1
PERSIST_GOVERNOR=1
DO_REBOOT=0

usage() {
  cat <<EOF
Usage: $0 [user@pi-host] [options]

Apply verified Raspberry Pi 3B low-latency OS, network, audio, and selective
hardware codec overclock tunings (idempotent, with automatic config backups).

Modes:
  (default)             Run remotely over SSH against ${PI_TARGET}
                        (override target via argument or PI_TARGET in .env.local)
  --local               Run directly on the Raspberry Pi itself

Options:
  --dry-run             Print planned changes without modifying system files
  --no-oc               Skip selective core/h264 overclock (keep gpu_mem=64 only)
  --no-persist-gov      Set CPU governor to performance now without systemd unit
  --reboot              Reboot the Pi automatically at the end if boot files changed
  -h, --help            Show this help

Tunings applied:
  1. /boot/firmware/config.txt : gpu_mem=64, disable_fw_kms_setup=1, and (unless --no-oc)
                                 core_freq=450, h264_freq=400, over_voltage=2
  2. /boot/firmware/cmdline.txt: smsc95xx.turbo_mode=N (disables USB-LAN RX batching)
  3. /etc/sysctl.d/99-moonlight-rpi3.conf: rmem_max=4194304, rmem_default=2097152,
                                           netdev_max_backlog=4096
  4. CPU governor               : performance (immediate + optional systemd oneshot)
  5. PipeWire / WirePlumber     : set HDMI (vc4-hdmi) sink as default audio output
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --local)
      LOCAL_MODE=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-oc)
      ENABLE_OC=0
      shift
      ;;
    --no-persist-gov)
      PERSIST_GOVERNOR=0
      shift
      ;;
    --reboot)
      DO_REBOOT=1
      shift
      ;;
    -h|--help)
      usage
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

# Auto-switch to local execution when invoked directly on a Raspberry Pi
if [[ "${LOCAL_MODE}" -eq 0 && -r /proc/device-tree/model ]] && grep -qi "Raspberry Pi" /proc/device-tree/model 2>/dev/null; then
  LOCAL_MODE=1
fi

read -r -d '' REMOTE_SCRIPT <<'EOS' || true
set -euo pipefail

DRY_RUN="${1}"
ENABLE_OC="${2}"
PERSIST_GOVERNOR="${3}"
DO_REBOOT="${4}"

REBOOT_NEEDED=0

run_sudo() {
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "[dry-run] sudo $*"
    return 0
  fi
  sudo "$@"
}

find_boot_file() {
  local name="$1"
  if [[ -f "/boot/firmware/${name}" ]]; then
    echo "/boot/firmware/${name}"
  elif [[ -f "/boot/${name}" ]]; then
    echo "/boot/${name}"
  else
    return 1
  fi
}

backup_once() {
  local file="$1"
  local bak="${file}.bak-pre-pi-tunning"
  if [[ ! -f "${bak}" ]]; then
    echo "==> Creating backup: ${bak}"
    run_sudo cp "${file}" "${bak}"
  fi
}

# Update or append key=value in config.txt inside/after [all] (avoiding [cm4]/[pi5] sections)
set_config_kv() {
  local file="$1"
  local key="$2"
  local val="$3"
  local desired="${key}=${val}"

  if grep -Eq "^[[:space:]]*${key}=${val}([[:space:]]*(#.*)?)?$" "${file}"; then
    echo "  [ok] ${desired} already set"
    return 0
  fi

  REBOOT_NEEDED=1
  echo "  [set] ${desired} in ${file}"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    return 0
  fi

  sudo python3 - "${file}" "${key}" "${val}" <<'PYEOF'
import re
import sys

path, key, val = sys.argv[1], sys.argv[2], sys.argv[3]
desired = f"{key}={val}\n"

with open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

pattern = re.compile(rf"^\s*{re.escape(key)}\s*=")
section = "[all]"
replaced = False
out = []

for line in lines:
    stripped = line.strip()
    if stripped.startswith("[") and stripped.endswith("]"):
        section = stripped.lower()
    if section == "[all]" and pattern.match(line):
        if not replaced:
            out.append(desired)
            replaced = True
        continue
    out.append(line)

if not replaced:
    if out and not out[-1].endswith("\n"):
        out[-1] += "\n"
    if not any(l.strip().lower() == "[all]" for l in out):
        out.append("\n[all]\n")
    out.append(desired)

with open(path, "w", encoding="utf-8") as f:
    f.writelines(out)
PYEOF
}

CONFIG_TXT="$(find_boot_file config.txt)" || { echo "Error: config.txt not found" >&2; exit 1; }
CMDLINE_TXT="$(find_boot_file cmdline.txt)" || { echo "Error: cmdline.txt not found" >&2; exit 1; }

echo "==> Configuring ${CONFIG_TXT}..."
backup_once "${CONFIG_TXT}"
set_config_kv "${CONFIG_TXT}" "gpu_mem" "64"
set_config_kv "${CONFIG_TXT}" "disable_fw_kms_setup" "1"
if [[ "${ENABLE_OC}" -eq 1 ]]; then
  set_config_kv "${CONFIG_TXT}" "core_freq" "450"
  set_config_kv "${CONFIG_TXT}" "h264_freq" "400"
  set_config_kv "${CONFIG_TXT}" "over_voltage" "2"
else
  echo "  [skip] Selective overclock skipped (--no-oc)"
fi

echo "==> Configuring ${CMDLINE_TXT}..."
backup_once "${CMDLINE_TXT}"
if grep -qw "smsc95xx.turbo_mode=N" "${CMDLINE_TXT}"; then
  echo "  [ok] smsc95xx.turbo_mode=N already present"
else
  REBOOT_NEEDED=1
  echo "  [set] Appending smsc95xx.turbo_mode=N to ${CMDLINE_TXT}"
  if [[ "${DRY_RUN}" -eq 0 ]]; then
    sudo python3 - "${CMDLINE_TXT}" <<'PYEOF'
import re
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    text = f.read().strip()

# Remove any prior smsc95xx.turbo_mode setting before appending N
tokens = [t for t in text.split() if not re.match(r"^smsc95xx\.turbo_mode=", t)]
tokens.append("smsc95xx.turbo_mode=N")
with open(path, "w", encoding="utf-8") as f:
    f.write(" ".join(tokens) + "\n")
PYEOF
  fi
fi

SYSCTL_CONF="/etc/sysctl.d/99-moonlight-rpi3.conf"
SYSCTL_CONTENT="# Moonlight Embedded low-latency UDP receive buffers (RPi 3B)
net.core.rmem_max = 4194304
net.core.rmem_default = 2097152
net.core.netdev_max_backlog = 4096"

echo "==> Configuring ${SYSCTL_CONF}..."
if [[ -f "${SYSCTL_CONF}" ]] && [[ "$(cat "${SYSCTL_CONF}")" == "${SYSCTL_CONTENT}" ]]; then
  echo "  [ok] ${SYSCTL_CONF} already up to date"
else
  echo "  [set] Writing ${SYSCTL_CONF}"
  if [[ "${DRY_RUN}" -eq 0 ]]; then
    printf "%s\n" "${SYSCTL_CONTENT}" | sudo tee "${SYSCTL_CONF}" >/dev/null
    sudo /sbin/sysctl -p "${SYSCTL_CONF}" >/dev/null || sudo sysctl --system >/dev/null
  fi
fi

echo "==> Setting CPU scaling governor to performance..."
if [[ "${DRY_RUN}" -eq 0 ]]; then
  for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [[ -w "${gov}" ]] || true
    echo performance | sudo tee "${gov}" >/dev/null
  done
else
  echo "  [dry-run] echo performance > /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
fi

if [[ "${PERSIST_GOVERNOR}" -eq 1 ]]; then
  GOV_UNIT="/etc/systemd/system/moonlight-cpufreq.service"
  GOV_CONTENT="[Unit]
Description=Set CPU frequency governor to performance for low-latency Moonlight streaming
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do [ -f \"\$g\" ] && echo performance > \"\$g\"; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target"
  if [[ -f "${GOV_UNIT}" ]] && [[ "$(cat "${GOV_UNIT}")" == "${GOV_CONTENT}" ]]; then
    echo "  [ok] ${GOV_UNIT} already installed"
  else
    echo "  [set] Installing and enabling ${GOV_UNIT}"
    if [[ "${DRY_RUN}" -eq 0 ]]; then
      printf "%s\n" "${GOV_CONTENT}" | sudo tee "${GOV_UNIT}" >/dev/null
      sudo systemctl daemon-reload
      sudo systemctl enable moonlight-cpufreq.service >/dev/null
    fi
  fi
fi

echo "==> Checking PipeWire/WirePlumber HDMI audio default..."
USER_UID="$(id -u)"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/${USER_UID}}"
if command -v wpctl >/dev/null 2>&1 && [[ -d "${RUNTIME_DIR}/pipewire-0" || -S "${RUNTIME_DIR}/pipewire-0" ]]; then
  HDMI_ID="$(XDG_RUNTIME_DIR="${RUNTIME_DIR}" wpctl status 2>/dev/null | awk '/Sinks:/{in_sinks=1; next} in_sinks && /Sources:|Filters:|Streams:|^[[:space:]]*$/{if (/Sources:|Filters:|Streams:/) in_sinks=0} in_sinks && /HDMI|vc4hdmi/ { if (match($0, /[0-9]+\./)) { print substr($0, RSTART, RLENGTH-1); exit } }')"
  if [[ -n "${HDMI_ID}" ]]; then
    echo "  [set] Setting WirePlumber default sink to HDMI (id=${HDMI_ID})"
    if [[ "${DRY_RUN}" -eq 0 ]]; then
      XDG_RUNTIME_DIR="${RUNTIME_DIR}" wpctl set-default "${HDMI_ID}"
    fi
  else
    echo "  [skip] No HDMI sink detected in wpctl status"
  fi
else
  echo "  [skip] WirePlumber/PipeWire session not active"
fi

echo "==> Current Pi status:"
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
cat /sys/module/smsc95xx/parameters/turbo_mode 2>/dev/null || true
/sbin/sysctl net.core.rmem_max net.core.rmem_default net.core.netdev_max_backlog 2>/dev/null || true
vcgencmd measure_temp 2>/dev/null || true
vcgencmd get_throttled 2>/dev/null || true

if [[ "${REBOOT_NEEDED}" -eq 1 ]]; then
  if [[ "${DO_REBOOT}" -eq 1 && "${DRY_RUN}" -eq 0 ]]; then
    echo "==> Boot configuration changed; rebooting Pi..."
    sudo reboot
  else
    echo "==> Boot configuration changed. Reboot the Pi for config.txt / cmdline.txt changes to take effect."
  fi
else
  echo "==> Done (no reboot required)."
fi
EOS

if [[ "${LOCAL_MODE}" -eq 1 ]]; then
  bash -s -- "${DRY_RUN}" "${ENABLE_OC}" "${PERSIST_GOVERNOR}" "${DO_REBOOT}" <<<"${REMOTE_SCRIPT}"
else
  SSH_CONTROL_DIR="${HOME}/.ssh/cm_socket"
  mkdir -p -m 700 "${SSH_CONTROL_DIR}"
  SSH_OPTS=(
    -F /dev/null
    -o "StrictHostKeyChecking=no"
    -o "ControlMaster=auto"
    -o "ControlPath=${SSH_CONTROL_DIR}/%r@%h:%p"
    -o "ControlPersist=10m"
  )
  echo "==> Running pi-tunning on ${PI_TARGET}..."
  ssh "${SSH_OPTS[@]}" "${PI_TARGET}" \
    "bash -s -- '${DRY_RUN}' '${ENABLE_OC}' '${PERSIST_GOVERNOR}' '${DO_REBOOT}'" <<<"${REMOTE_SCRIPT}"
fi
