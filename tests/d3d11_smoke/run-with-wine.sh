#!/usr/bin/env bash
# Reproduces a D3D11-over-Vulkan (DXVK) workload with and without the layer.
#
#   bash tests/d3d11_smoke/run-with-wine.sh [W H draws]
#
# Builds tests/d3d11_smoke/main.cpp with mingw-w64, sets up a wine prefix with
# DXVK (nixpkgs wine + dxvk) and runs the program twice: plain, and with the
# framefov layer in standalone mode. Both runs must succeed; the second one
# foveates (see the layer log).
#
# Needs Nix (nix-shell/nix build) for mingw-w64, wine and DXVK.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="${FRAMEFOV_D3D11_WORK:-/tmp/framefov-d3d11}"
width="${1:-2432}"
height="${2:-2304}"
draws="${3:-256}"
prefix="$work/wine"
mkdir -p "$work"

echo "== building the D3D11 smoke test with mingw-w64"
nix-shell -p pkgsCross.mingwW64.stdenv.cc --run \
  "x86_64-w64-mingw32-g++ -O1 -std=c++17 -o $work/d3d11_smoke.exe $here/main.cpp \
   -ld3d11 -ldxgi -ld3dcompiler -lole32 -luuid -static-libgcc -static-libstdc++" || exit 1

if [[ ! -d "$prefix/drive_c/windows/system32" ]]; then
  echo "== creating the wine prefix and installing DXVK"
  dxvk="$(nix build --no-link --print-out-paths nixpkgs#dxvk 2>/dev/null | tail -1)"
  [[ -n "$dxvk" ]] || { echo "could not build dxvk" >&2; exit 1; }
  WINEPREFIX="$prefix" WINEARCH=win64 nix-shell -p wineWow64Packages.stable --run \
    "wineboot -u >/dev/null 2>&1; cp '$dxvk'/x64/*.dll '$prefix/drive_c/windows/system32/'" || exit 1
fi

run() {  # $1 = label, rest = extra env for the layer
  local label="$1"; shift
  echo
  echo "== $label (${width}x${height}, ${draws} draws/frame)"
  ( cd "$work" && env WINEPREFIX="$prefix" WINEDEBUG=-all \
      WINEDLLOVERRIDES="d3d11,dxgi,d3d10core=n" \
      LD_LIBRARY_PATH="/run/opengl-driver/lib:${LD_LIBRARY_PATH:-}" \
      VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/run/opengl-driver/share/vulkan/icd.d/radeon_icd.x86_64.json}" \
      XDG_STATE_HOME="$work/state" \
      "$@" \
      nix-shell -p wineWow64Packages.stable --run \
      "wine ./d3d11_smoke.exe $width $height $draws" 2>&1 | tail -3 )
}

run "plain DXVK" FRAMEFOV_DISABLE=1
run "with framefov (standalone fixed foveation)" \
  FRAMEFOV_ENABLE=1 FRAMEFOV_STANDALONE=1 "FRAMEFOV_EYE_SIZE=${width}x${height}" \
  FRAMEFOV_LOG_LEVEL=debug

echo
echo "layer log: $work/state/framefov/framefov.log"
strings "$work/state/framefov/framefov.log" 2>/dev/null | grep -m3 -E "synthetic snapshot|ACTIVE|attaching" || true
