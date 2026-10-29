{ pkgs }:

# macOS builds target x86-64 and run under Rosetta 2 (README, "Building on macOS"). They use
# Xcode's clang and SDK, so this shell only adds the build tools: nixpkgs' compiler wrapper and
# SDK would get in the way of -DCMAKE_OSX_ARCHITECTURES=x86_64. MoltenVK and an x86-64 capable Qt
# still come from outside the shell, as the README describes.
pkgs.mkShellNoCC {
  name = "kytyps5";

  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    git
    glslang # provides glslangValidator for the bundled shaders
    python3
  ];

  shellHook = ''
    # Xcode's toolchain, not one from the store
    export DEVELOPER_DIR="$(/usr/bin/xcode-select -p)"
    unset SDKROOT

    if [ -z "''${KYTY_SHELL_QUIET:-}" ]; then
      echo "KytyPS5 dev shell (macOS)"
      echo "  cmake -S . -B _Build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \\"
      echo "    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_C_COMPILER=/usr/bin/clang \\"
      echo "    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DKYTY_BUILD_LAUNCHER=OFF"
      echo "  cmake --build _Build/macos --target kyty_emulator --parallel"
    fi
  '';
}
