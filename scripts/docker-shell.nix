# bbcoop, container builds only (scripts/docker-test.sh): shell.nix plus Vulkan-Headers 1.4.350,
# which has VK_VALVE_shader_mixed_float_dot_product (vk_instance.cpp); nixpkgs had 1.4.341 on
# 2026-10-07. Listed first so CMake's FindVulkan picks these headers; nothing is rebuilt.
# gdb is for debugging test clients in the container. SDL3 is built from source here (shell.nix
# overrides it), and its own testprocess fails under amd64 emulation (2026-10-27), so its checks are off.
{ pkgs ? import <nixpkgs> {
    overlays = [ (final: prev: { sdl3 = prev.sdl3.overrideAttrs { doCheck = false; }; }) ];
  } }:
let
  base = import ../shell.nix { inherit pkgs; };
  headers = pkgs.vulkan-headers.overrideAttrs (old: {
    version = "1.4.350.0";
    src = builtins.fetchTarball "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.0.tar.gz";
  });
in
base.overrideAttrs (old: {
  nativeBuildInputs = [ headers pkgs.gdb ] ++ (old.nativeBuildInputs or []);
})
