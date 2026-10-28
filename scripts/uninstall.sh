#!/usr/bin/env bash
# Remove a user (default) or system install created by scripts/install.sh or
# scripts/install-frame.sh.
#
#   scripts/uninstall.sh [--system] [--prefix DIR] [--keep-config]
set -euo pipefail

prefix="${FRAMEFOV_PREFIX:-$HOME/.local}"
keep_config=0
system=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --system) prefix="/usr"; system=1; shift ;;
    --prefix) prefix="$2"; shift 2 ;;
    --prefix=*) prefix="${1#*=}"; shift ;;
    --keep-config) keep_config=1; shift ;;
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

rm -f "$prefix/share/vulkan/implicit_layer.d/VkLayer_framefov.json"
rm -rf "$prefix/lib/framefov"

if [[ $system -eq 0 ]]; then
  unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
  rm -f "$unit_dir/framefov-gaze.service"
  rm -rf "$unit_dir/framefov-gaze.service.d"

  if [[ -n "${XDG_RUNTIME_DIR:-}" ]] && command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload 2>/dev/null || true
  fi
fi

if [[ $keep_config -eq 0 ]]; then
  config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/framefov"
  rm -f "$config_dir/settings.ini"
  rmdir "$config_dir" 2>/dev/null || true
fi

echo "uninstalled framefov from $prefix"
