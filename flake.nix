# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
{
  description = "Jam compacting generational garbage collector for C++26";

  inputs = {
    native.url = "git+https://github.com/ekmett/native?ref=main&shallow=1";
    nixpkgs.follows = "native/nixpkgs";
    work = {
      url = "git+https://github.com/ekmett/work?ref=main&shallow=1";
      flake = false;
    };
  };

  outputs = { self, native, nixpkgs, work, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages_23;
          nativePackage = native.packages.${system}.native.overrideAttrs (old: {
            cmakeFlags = old.cmakeFlags ++ [ "-DNATIVE_ENABLE_EXCEPTIONS=ON" ];
          });
          # Share Native's installed Hint package with Work and Jam.
          hintPackages = nativePackage.propagatedBuildInputs;
          workPackage = llvm.stdenv.mkDerivation {
            pname = "work";
            version = "0.1.0";
            src = work;
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja llvm.clang-tools ];
            propagatedBuildInputs = hintPackages;
            cmakeFlags = [
              "-DWORK_BUILD_TESTS=OFF"
              "-DWORK_BUILD_DOCS=OFF"
              "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            ];
          };
          dependencies = [ nativePackage workPackage ] ++ hintPackages;
          jam = llvm.stdenv.mkDerivation {
            pname = "jam";
            version = "0.0.1";
            src = pkgs.lib.fileset.toSource {
              root = ./.;
              fileset = pkgs.lib.fileset.unions [
                ./CMakeLists.txt ./LICENSE.md ./LICENSE-BSD-2-Clause.md ./LICENSE-APACHE.md
                ./THIRD_PARTY_NOTICES.md ./src ./etc/cmake ./t/installed
              ];
            };
            nativeBuildInputs = [ pkgs.cmake pkgs.ninja llvm.clang-tools ];
            propagatedBuildInputs = dependencies;
            # Nix's Clang wrapper adds linker flags to CMake BMI --precompile steps.
            env.NIX_CFLAGS_COMPILE = "-Wno-unused-command-line-argument";
            cmakeFlags = [
              "-DJAM_BUILD_TESTS=OFF"
              "-DJAM_BUILD_DOCS=OFF"
              "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
            ];
            doInstallCheck = true;
            installCheckPhase = ''
              runHook preInstallCheck
              cmake -S "$src/t/installed" -B consumer -G Ninja \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_PREFIX_PATH="$out;${pkgs.lib.concatStringsSep ";" dependencies}" \
                -DFETCHCONTENT_FULLY_DISCONNECTED=ON
              cmake --build consumer --parallel "$NIX_BUILD_CORES"
              ctest --test-dir consumer --output-on-failure --no-tests=error
              runHook postInstallCheck
            '';
            meta = {
              description = "Compacting generational garbage collector for C++26";
              homepage = "https://github.com/ekmett/jam";
              license = with pkgs.lib.licenses; [ bsd2 asl20 ];
              platforms = systems;
            };
          };
        in { inherit jam; default = jam; });
      checks = forAllSystems (system: { inherit (self.packages.${system}) jam; });
      devShells = forAllSystems (system:
        let pkgs = import nixpkgs { inherit system; };
        in {
          default = (pkgs.mkShell.override { stdenv = pkgs.llvmPackages_23.stdenv; }) {
            inputsFrom = [ self.packages.${system}.jam ];
          };
        });
    };
}
