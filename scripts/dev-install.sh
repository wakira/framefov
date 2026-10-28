#!/usr/bin/env bash
# Development helper: rebuild-free sync of the freshly built library into the
# user install, so the loader (which reads the manifest from the passwd home
# directory, not $HOME) picks up the current build.
#
#   scripts/dev-install.sh [--build-dir DIR] [--prefix DIR]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"
build_dir="${FRAMEFOV_BUILD_DIR:-$repo_dir/build}"
prefix="${FRAMEFOV_PREFIX:-$HOME/.local}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) build_dir="$2"; shift 2 ;;
    --build-dir=*) build_dir="${1#*=}"; shift ;;
    --prefix) prefix="$2"; shift 2 ;;
    --prefix=*) prefix="${1#*=}"; shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

lib="$build_dir/libVkLayer_framefov.so"
if [[ ! -f "$lib" ]]; then
  echo "error: $lib not found; build first" >&2
  exit 1
fi

manifest="$prefix/share/vulkan/implicit_layer.d/VkLayer_framefov.json"
if [[ ! -f "$manifest" ]]; then
  echo "manifest missing; running scripts/install.sh first"
  "$script_dir/install.sh" --prefix "$prefix" --build-dir "$build_dir"
fi

install -D -m 0755 "$lib" "$prefix/lib/framefov/libVkLayer_framefov.so"
echo "synced $lib -> $prefix/lib/framefov/libVkLayer_framefov.so"
