# Reference environment: WolfLutris

The reference container is `gameonwhales/lutris:kaijuu` (Ubuntu 25.04) with an
AMD RX 9060 XT passed through. It is a **test environment**, not a requirement:
the generic build in [BUILD](BUILD.md) applies to any distro with the right
Mesa/Vulkan and a VapourSynth-enabled mpv.

## Why extra setup is needed

| Problem | Cause | Fix |
|---|---|---|
| No `coop_matrix` / `shader_float8` | distro Mesa is 25.0.7 | install DLSSNR-AMD's Mesa 25.3.3 debs (`fix-mesa.sh` in the DLSSNR package) |
| apt refuses every install after Mesa | `dpkg -i --force-depends` leaves unmet `mesa-libgallium` / `libwayland` deps | metadata-only shim debs (done inside `scripts/build-vapoursynth-mpv.sh`) |
| `wl_display_dispatch_queue_timeout` undefined | Mesa 25.3.3 needs libwayland **1.24**, container has 1.23.1 | `scripts/fix-wayland.sh` (questing 1.24.0 debs + `libxml2-16`) |
| mpv has no `--vf=vapoursynth` | Ubuntu mpv is built without VS | `scripts/build-vapoursynth-mpv.sh` (VS R81 + mpv 0.41) |
| mpv 0.41 cannot load VS | it links `libvsscript`, not dlopen; a symlink breaks `dladdr()` | real path in `/etc/ld.so.conf.d/vapoursynth.conf` + `ldconfig`; `python3 -m vapoursynth config` |

`scripts/fix-wayland.sh` is idempotent and has a `--check` mode. It downloads the
questing debs, verifies SHA-256, backs up the old packages under
`/home/retro/wayland-rollback/1.23.1/`, and prints the rollback command.

## Restore order after a container rebuild

```bash
# 0) Mesa 25.3.3 (package from the DLSSNR-AMD setup, with --force-depends)
docker exec -u root <container> bash /home/retro/dlssnr-test/fix-mesa.sh

# 1) libwayland 1.24 (display path; must be after Mesa or before, as long as both are installed)
docker exec -u root <container> bash /home/retro/dnr/repo/scripts/fix-wayland.sh

# 2) VS R81 + mpv 0.41 (also installs the apt shim)
docker exec -u root -w /home/retro/dnr <container> \
    bash repo/scripts/build-vapoursynth-mpv.sh --prefix /usr/local

# 3) core + plugin
docker exec -u root -w /home/retro/dnr <container> bash repo/scripts/build-core.sh
docker exec -u root -w /home/retro/dnr <container> \
    env DNR_LIBRARY=/home/retro/dnr/build/libdnr.so \
    bash repo/scripts/build-vs-plugin.sh --prefix /usr/local
docker exec -u root <container> \
    ln -sfn /home/retro/dnr/build/libdnr.so /usr/local/lib/vapoursynth/libdnr.so
```

Then verify:

```bash
docker exec -u retro <container> bash -lc '
  export XDG_RUNTIME_DIR=/run/user/wolf WAYLAND_DISPLAY=wayland-2
  export DNR_ROOT=/home/retro/dlssnr-test
  export DNR_PLUGIN=/usr/local/lib/vapoursynth/dnr.so
  mpv --no-config --no-audio --vo=gpu-next --gpu-api=vulkan --length=5 \
      --vf=vapoursynth=/home/retro/dnr/repo/dnr.vpy /home/retro/dnr/sample.mp4'
```

## Notes

- `/home/retro` is a bind mount; `/usr/local` is in the container layer and is
  lost on rebuild. Re-run the scripts above (~2–3 min).
- `pipeline.cache` is written under `/home/retro/dlssnr-test/dlssnr-amd/`
  (persisted).
- The container's `fix-mesa.sh` still uses `--force-depends`; running
  `fix-wayland.sh` afterwards is required for GPU display. Ideally the image
  should be based on Ubuntu 25.10+ where both Mesa and libwayland are new enough.
- Do not modify Steam/Lutris game installs for this player; it only reads the
  `dlssnr-amd/` model root.

## Rollback

```bash
# libwayland
cd /home/retro/wayland-rollback/1.23.1 && dpkg -i ./*.deb && ldconfig

# Mesa
apt-get install --allow-downgrades -y \
  mesa-vulkan-drivers=25.0.7-0ubuntu0.25.04.2 \
  mesa-vulkan-drivers:i386=25.0.7-0ubuntu0.25.04.2
```

After rollback, GPU display via Vulkan stops working again (headless compute
still works), until libwayland is upgraded.
