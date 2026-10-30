# S7D DRM writeback test

Capture the real S7D VPU through the standard DRM writeback connector. This
runtime-mode tool includes the original fixed-mode suite: repeated target
replacement, CTM/gamma, invalid target and source-alias checks, exact RGB8 pixel
comparison, explicit writeback and CRTC fences, guard rows and state restoration.

Build from a C5 kernel checkout, using its DRM UAPI headers and host/AArch64 libc development headers:

```sh
make KERNEL_SRC=/path/to/common CROSS_COMPILE=aarch64-linux-gnu- O=/tmp/c5-wb-build
make KERNEL_SRC=/path/to/common O=/tmp/c5-wb-build check
```

`CC`, `CROSS_CC`, `CROSS_COMPILE`, `CFLAGS`, `KERNEL_SRC` and `O` are configurable.
The build links the project DRM headers under the output directory; Linux/asm
userspace types come from the selected libc toolchain. No libdrm, GPU or
vendor userspace dependency is required. `sha256sum` must be
available at `/usr/bin/sha256sum` on the board for kernel-notes verification.

Before running, stop the compositor and confirm the original display belongs
to fbcon or is inactive. Supply independently verified boot and kernel pins:

```sh
sudo /tmp/c5-wb-build/writeback-test-aarch64 --run \
  --expected-boot PINNED_UUID --expected-notes PINNED_NOTES_SHA256 \
  --route headless --mode vu12 --output /var/tmp/NEW_RESULT
```

`--route clone` keeps HDMI attached, `headless` detaches HDMI and captures the
real VPU without a sink, and `both` runs both paths. Clone requires the sink to
advertise the requested mode. `--probe` performs read-only discovery without
DRM master or framebuffer changes; it needs the same identity pins.

Presets: `vu12` (1920x720), `1080p60`, `fhd120`, `4k30`, `4k60`, `dci4k60`.
The driver must admit each preset; a listed preset is not a hardware support or
performance claim. `--smoke 1|2` reduces the suite to baseline captures with
one/two targets. `--mode-sequence vu12,fhd120,vu12` tests transitions in one
headless process while retaining the old source until the new capture retires.
Without `--smoke`, each selected route runs baseline, replacement, CTM, orange
and blue gamma captures plus separate TEST_ONLY negatives.

Output includes runtime mode/route metadata, measurements and packed RGB8
capture files in a new absolute directory. Pixel matches concern the admitted
VPU profile; they do not establish HDMI transmission, optical output, HDR or
10/12-bit precision. The source buffer remains immutable during capture.

An unsignaled writeback fence or incomplete restoration enters `RECOVERY_HOLD`
and retains master, buffers and blobs. Inspect and recover that device before
releasing the process. A normal finite exit verifies restoration and releases
owned resources; resume the compositor afterwards.

## Capture storage depth

`--capture-format bgr888|xbgr2101010` selects the target format advertised by
the running writeback connector. The default remains `bgr888`. The source
remains immutable XRGB8888 and the physical HDMI request remains RGB8; no
writeback `max bpc` property is required.

BGR888 uses three memory bytes R/G/B. XBGR2101010 uses four little-endian bytes
with R in bits0–9, G10–19 and B20–29; X30–31 are ignored by the comparison.
Baseline and CTM references expand the RGB8 values by two bits. Constant gamma
references follow `drm_color_lut_extract(value, 10)`: orange is1023/256/64 and
blue64/256/1023. These constants are not the RGB8 reference shifted by two.
The hardware path must retain the aligned12-bit gamma output through the
10-bit capture stage; the tool reports mismatches if that assumption fails.

The complete suite and smoke/transition options work with either target. Guard
rows and padding follow its three/four-byte pixel layout. Dumps contain active
rows without row padding and preserve the captured bytes: `.rgb` for BGR888,
`.xbgr2101010` for XBGR2101010. `PIXELS` reports stored row bytes, native pitch,
format and component depth. Target X bits remain in the raw dump.

This tests SDR RGB8 input and10-bit gamma output storage. It does not qualify a
10-bit source producer, arbitrary color precision, HDR metadata or HDMI deep
color transmission.
