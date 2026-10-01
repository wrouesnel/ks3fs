# GNU hello compiled from source with a pinned nixpkgs stdenv, without
# evaluating nixpkgs: the stdenv closure is substituted from cache.nixos.org
# (into the ks3fs-backed store), the output has a name of its own so it can
# only be built, never substituted.
{ stdenv, shell, src }:
derivation {
  name = "hello-ks3fs";
  system = "x86_64-linux";
  builder = shell;
  args = [ "-c" "source $stdenv/setup; genericBuild" ];
  stdenv = builtins.storePath stdenv;
  src = builtins.path { path = src; name = "hello-src.tar.gz"; };
}
