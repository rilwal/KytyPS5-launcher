{
  description = "KytyPS5 development shell";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-darwin" "x86_64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default =
            if pkgs.stdenv.hostPlatform.isDarwin
            then import ./nix/devshell-darwin.nix { inherit pkgs; }
            else import ./nix/devshell.nix { inherit pkgs; };
        });
    };
}
