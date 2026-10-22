{
  description = "miniosv — slim unikernel OS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";

    # The `cwd` slot is a special app whose default is a sentinel that
    # refuses to build; override it with --override-input to point at a
    # local tree:
    #   nix build --override-input cwd "path:$PWD" \
    #       github:miniosv/miniosv#cwd-x86_64
    # Or in a downstream flake:
    #   inputs.miniosv.url = "github:miniosv/miniosv";
    #   inputs.miniosv.inputs.cwd.url = "path:./my-app";
    cwd = {
      url = "path:./nix/cwd-sentinel";
      flake = false;
    };

    # QEMU carrying the vaccel device, for modules/vaccel.
    lros-qemu.url = "github:TUM-DSE/lros-qemu/master+vaccel+modern";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      cwd,
      lros-qemu,
    }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        arch = if system == "x86_64-linux" then "x86_64" else "aarch64";

        upstream = import ./nix {
          inherit nixpkgs system self;
          apps = {
            default = ./app;
            cwd = cwd;
          };
        };

        # On x86 the aarch64 firmware as well, so a cross-built image can be
        # run from the workstation.
        crossFirmware = pkgs.lib.optionalAttrs (system == "x86_64-linux") {
          AAVMF_CODE = "${pkgs.qemu}/share/qemu/edk2-aarch64-code.fd";
          AAVMF_VARS = pkgs.runCommand "aavmf-vars.fd" { } "install -m444 /dev/null $out";
        };

        # What this fork adds to every shell: e2fsprogs, for the miniext
        # staging scripts, and cargo and rustc, for modules/mininet (a Rust
        # crate every x64 image builds).
        srg =
          shell:
          shell.overrideAttrs (
            old:
            { nativeBuildInputs = [ pkgs.e2fsprogs pkgs.cargo pkgs.rustc ] ++ old.nativeBuildInputs; } // crossFirmware
          );

        # The vaccel QEMU, for modules/vaccel. Kept off the `build` shell:
        # pointing at it means building it, and a builder has no use for it.
        withVaccel =
          shell:
          shell.overrideAttrs (old: {
            QEMU_VACCEL = "${lros-qemu.packages.${system}.qemu-vaccel}/bin/qemu-system-${arch}";
          });
      in
      upstream
      // {
        devShells = rec {
          default = withVaccel build;
          # No vaccel QEMU here, so this one can be entered without building it.
          build = srg upstream.devShells.default;
          aws = withVaccel (srg upstream.devShells.aws);
          cli = withVaccel (srg upstream.devShells.cli);
          # The Rust-carrying modules (mininet) need cargo.
          rust = cli.overrideAttrs (old: {
            nativeBuildInputs = [ pkgs.cargo ] ++ old.nativeBuildInputs;
          });
        };
      }
    );
}
