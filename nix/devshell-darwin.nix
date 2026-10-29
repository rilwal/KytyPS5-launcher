{ pkgs }:

# macOS builds target x86-64 and run under Rosetta 2 (README, "Building on macOS"). They use
# Xcode's clang, SDK and codesign, so this shell only adds the build tools: nixpkgs' compiler
# wrapper and SDK would get in the way of -DCMAKE_OSX_ARCHITECTURES=x86_64. MoltenVK and an x86-64
# capable Qt still come from outside the shell, as the README describes.
pkgs.mkShellNoCC {
  name = "kytyps5";

  nativeBuildInputs = with pkgs; [
    ccache # CMake picks up any ccache on PATH; this keeps it from coming from Homebrew
    cmake
    ninja
    git
    glslang # provides glslangValidator for the bundled shaders (a host tool, so arm64 is fine)
    python3
  ];

  shellHook = ''
    # Xcode's toolchain, not one from the store; a DEVELOPER_DIR picking a specific Xcode stays.
    if [ -z "''${DEVELOPER_DIR:-}" ] || [[ "$DEVELOPER_DIR" == /nix/store/* ]]; then
      if dev="$(/usr/bin/xcode-select -p 2>/dev/null)"; then
        export DEVELOPER_DIR="$dev"
      else
        echo "warning: Xcode / Command Line Tools not found (xcode-select --install)" >&2
      fi
    fi
    unset SDKROOT

    if [ -z "''${KYTY_SHELL_QUIET:-}" ]; then
      echo "KytyPS5 dev shell (macOS)"
      echo "  cmake -S . -B _Build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \\"
      echo "    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_C_COMPILER=/usr/bin/clang \\"
      echo "    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ -DKYTY_BUILD_LAUNCHER=OFF"
      echo "  cmake --build _Build/macos --target kyty_emulator --parallel"
      echo "  cmake --install _Build/macos --prefix _Build/macos/install"
      echo "  # With Kyty's Qt launcher (KytyPS5.app): drop -DKYTY_BUILD_LAUNCHER=OFF,"
      echo "  # add -DCMAKE_PREFIX_PATH=\"\$Qt6_DIR\" and build --target launcher"
    fi
  '';
}
