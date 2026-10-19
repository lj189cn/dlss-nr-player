# VLC plugin: evaluation (no-go)

A VLC video-filter plugin was evaluated as an alternative frontend. Conclusion:
**technically possible, but not the primary path.** The mpv + VapourSynth route
has better frame scheduling and a cleaner API.

## Facts (Ubuntu 25.04, VLC 3.0.21)

| Item | Finding |
|---|---|
| Plugin headers | in **`libvlccore-dev`**, not `libvlc-dev` (`vlc/plugins/vlc_filter.h`, `vlc-plugin.pc`) |
| Entry symbol | `vlc_entry__3_0_0f` (VLC 3.0.x); VLC 4 changes the entry/ops/macros and needs a rewrite |
| Plugin path | `<libdir>/vlc/plugins` or `$VLC_PLUGIN_PATH`; there is **no** `~/.local/lib/vlc/plugins` autoload |
| Vulkan | VLC 3.0.x has **no Vulkan vout**; the filter gets CPU `picture_t` only |
| VLC 4 | has a Vulkan vout, but `vlc_decoder_device` / `video_context` still expose no Vulkan device to filters |
| Threading | the filter runs synchronously on the **vout thread**; 25 ms there blocks display |
| Zero-copy | impossible in both VLC 3 and 4 for a video filter |

## Cost estimate

- Prototype: 2–3 person-days (module skeleton, `sws_scale` both ways, `libdnr`,
  install/test).
- Productisation: +4–7 person-days.
- VLC 4 migration: +1–2 person-days when Ubuntu moves.

At 1080p the extra conversions put a frame at ~25–28 ms (≈36–40 fps), and a 25 ms
block on the vout thread hurts 60 Hz playback. 4K is 90–115 ms (unusable).

## When to revisit

- VLC becomes a hard requirement;
- the mpv route is unavailable on the target distro/hardware;
- a quick demo is needed before the mpv stack is built.

Stop line for a spike: if `in + NR + out > 30 ms` at 1080p, or VAAPI interop adds
more than 5 ms/frame, stop and keep mpv.

The full investigation (API details, code snippets, build/install commands,
source links) was produced during development; this repository keeps only the
summary because the route is not maintained.
