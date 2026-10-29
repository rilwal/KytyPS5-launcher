let
  pkgs = import <nixpkgs> { };
in
if pkgs.stdenv.hostPlatform.isDarwin
then import ./nix/devshell-darwin.nix { inherit pkgs; }
else import ./nix/devshell.nix { inherit pkgs; }
