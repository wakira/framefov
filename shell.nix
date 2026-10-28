# Convenience for NixOS: `nix-shell` uses <nixpkgs> directly (no flake fetch).
# Prefer `nix develop` for the pinned nixpkgs-unstable dev shell.
with import <nixpkgs> { };

mkShell {
  name = "framefov-dev";
  packages = [
    cmake
    ninja
    gcc
    gdb
    pkg-config
    python3
    glslang
    shaderc
    vulkan-headers
    vulkan-loader
    vulkan-tools
    vulkan-validation-layers
    openxr-loader
    openvr
  ];
  shellHook = ''
    export FRAMEFOV_USE_SYSTEM_HEADERS=ON
    export FRAMEFOV_VULKAN_INCLUDE_DIR="${vulkan-headers}/include"
    export FRAMEFOV_OPENXR_INCLUDE_DIR="${openxr-loader.dev}/include"
    export FRAMEFOV_OPENVR_INCLUDE_DIR="${openvr}/include"
    export FRAMEFOV_OPENVR_LIBRARY_DIR="${openvr}/lib"
    export FRAMEFOV_GLSLANG_VALIDATOR="${glslang}/bin/glslangValidator"
    echo "framefov dev shell (nix-shell)"
    echo "  cmake -B build -GNinja -DFRAMEFOV_USE_SYSTEM_HEADERS=ON"
  '';
}
