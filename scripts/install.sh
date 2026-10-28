#!/usr/bin/env bash
# Install framefov as a user (default) or system Vulkan layer.
#
#   scripts/install.sh [--system] [--prefix DIR] [--build-dir DIR]
#
# User install lays out:
#   <prefix>/lib/framefov/libVkLayer_framefov.so
#   <prefix>/lib/framefov/framefov-gaze               (if built)
#   <prefix>/share/vulkan/implicit_layer.d/VkLayer_framefov.json
#   ~/.config/framefov/settings.ini                   (only if absent)
#
# For the Steam Frame per-user layout (systemd gaze unit included), see
# scripts/install-frame.sh instead.
#
# System install is needed for Steam's pressure-vessel container, which imports
# host Vulkan layers rather than the user's ~/.local install.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"

prefix="${FRAMEFOV_PREFIX:-$HOME/.local}"
build_dir="${FRAMEFOV_BUILD_DIR:-$repo_dir/build}"
system=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --system) prefix="/usr"; system=1; shift ;;
    --prefix) prefix="$2"; shift 2 ;;
    --prefix=*) prefix="${1#*=}"; shift ;;
    --build-dir) build_dir="$2"; shift 2 ;;
    --build-dir=*) build_dir="${1#*=}"; shift ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

lib_src="$build_dir/libVkLayer_framefov.so"
if [[ ! -f "$lib_src" ]]; then
  echo "error: $lib_src not found; build first (cmake --build build)" >&2
  exit 1
fi

lib_dir="$prefix/lib/framefov"
vk_dir="$prefix/share/vulkan/implicit_layer.d"

install -d "$lib_dir" "$vk_dir"
install -m 0755 "$lib_src" "$lib_dir/libVkLayer_framefov.so"

lib_path="$lib_dir/libVkLayer_framefov.so"

sed "s|@FRAMEFOV_LIBRARY_PATH@|$lib_path|g" \
  "$repo_dir/manifests/VkLayer_framefov.json.in" > "$vk_dir/VkLayer_framefov.json"

gaze_src="$build_dir/framefov-gaze"
if [[ -f "$gaze_src" ]]; then
  install -m 0755 "$gaze_src" "$lib_dir/framefov-gaze"
fi

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/framefov"
if [[ ! -f "$config_dir/settings.ini" ]]; then
  install -d "$config_dir"
  cat > "$config_dir/settings.ini" <<'INI'
; framefov settings — see README.md for the full reference.
[global]
standalone = 1
eye_size = 2176x2160
inner_radius = 0.35
outer_radius = 0.70
inner_rate = 1x1
middle_rate = 2x2
outer_rate = 4x4
INI
  echo "wrote default config: $config_dir/settings.ini"
fi

echo "installed libVkLayer_framefov.so -> $lib_path"
echo "installed $vk_dir/VkLayer_framefov.json"
echo
echo "enable per application with:  FRAMEFOV_ENABLE=1 <app>"
echo "or as a Steam launch option:  FRAMEFOV_ENABLE=1 %command%"
if [[ $system -eq 0 ]]; then
  echo
  echo "note: for Proton/pressure-vessel games use --system, because Vulkan"
  echo "      layers are imported from the host."
fi
