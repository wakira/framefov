{
  description = "framefov — VRS-based foveated rendering layer for Linux OpenXR (Vulkan)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          name = "framefov-dev";
          packages = with pkgs; [
            cmake
            ninja
            gcc
            gdb
            pkg-config
            git
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
          # Headers come from nixpkgs instead of FetchContent (offline builds).
          shellHook = ''
            export FRAMEFOV_USE_SYSTEM_HEADERS=ON
            export FRAMEFOV_VULKAN_INCLUDE_DIR="${pkgs.vulkan-headers}/include"
            export FRAMEFOV_OPENXR_INCLUDE_DIR="${pkgs.openxr-loader.dev}/include"
            export FRAMEFOV_OPENVR_INCLUDE_DIR="${pkgs.openvr}/include"
            export FRAMEFOV_OPENVR_LIBRARY_DIR="${pkgs.openvr}/lib"
            export FRAMEFOV_GLSLANG_VALIDATOR="${pkgs.glslang}/bin/glslangValidator"
            echo "framefov dev shell"
            echo "  cmake -B build -GNinja -DFRAMEFOV_USE_SYSTEM_HEADERS=ON"
            echo "  cmake --build build && ctest --test-dir build --output-on-failure"
          '';
        };
      });
    };
}
