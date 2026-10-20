# ODROID-C5 / S7D native drivers

This project replaces the tightly coupled Amlogic display path with drivers organized around Linux kernel interfaces. The main improvements are:

- **Kernel-style organization:** separate DRM/KMS, HDMI bridge, audio and hardware-resource drivers with explicit responsibilities.
- **Hierarchical resource management:** describe clocks, resets, power domains and PHYs in Device Tree and connect drivers through standard provider/consumer interfaces.
- **Standard display and audio APIs:** use DRM/KMS, DMA-BUF and ASoC/HDMI codec interfaces in place of vendor-specific display controls; retain the existing kbase/libMali GPU ABI.
- **Linux-controlled modesetting:** select and program display timings through DRM instead of relying on U-Boot `outputmode` strings or a vendor timing-name whitelist.
- **Monitor replacement:** handle HDMI hotplug and refresh EDID/modes when a panel changes, with repeated hotplug and recovery qualification still in progress.
- **Native EDID discovery:** read supported monitor timings directly without routine EDID injection or panel-specific overrides; malformed or missing EDID can still require an override.
- **Streamlined initialization:** make display setup explicit and avoid unnecessary reinitialization; any boot-time improvement remains unquantified by a controlled vendor/native comparison.

Firmware/SMC services and retained hardware state are still required; independent cold initialization, complete power-off recovery and seamless U-Boot logo handoff remain unfinished.

## Tested displays

-  AlphaScan AOC 2269WM
-  LG HDR 4K (EDID name; retail model not recorded)
-  Hardkernel ODROID VU7A+
-  Hardkernel ODROID VU7+
-  Hardkernel ODROID VU5A
-  Hardkernel ODROID VU12

## Feature coverage

| Feature | Vendor / S7D | New driver | Scope / remaining work |
| --- | --- | :---: | --- |
| **Status — 2026-10-21** | S7D-specific implementation; not every feature is enabled on C5 | ✓ / △ / ✗ | ✓ Complete vendor/S7D feature coverage implemented and verified; △ partial, in progress, or not fully qualified; ✗ not implemented. A successful subset or source/build check alone does not earn ✓. |
| **Display implementation target** | All documented S7D display capabilities, including features disabled in the C5 vendor profile | △ | Source/calculation/build validation and selected board tests; complete vendor/S7D parity is still the target. Existing exclusions for protected/proprietary content and unused inputs remain. |
| **Latest board state** | Current validation and recovery | △ | Native build 138 on VU12 at EDID-selected 1920×720 / about 59.86Hz after normal reboot; original DT and libMali retained. VD2 packed VUY888 scaling passed 254 board presentations, event/fence completion and GNOME restoration. VD1 SAFA build 139 is source/build-checked only and is not running on the board. |
| DRM atomic KMS, TEST_ONLY | Supported | △ | One CRTC; 41 board TEST_ONLY requests left reported state unchanged, including two expected rejections. All 39 scenes completed event/fence updates after the readback fix; complete lifecycle and error-path qualification remains. |
| S7D timing and shared-resource admission | SoC limits and shared display blocks | △ | Common FHD120/UHD60 envelope and per-frame input, compression and scaler claims; display-fetch demand is modeled. Shared-DDR capacity qualification remains. |
| GEM, dumb buffers, DMA-BUF, fences | Supported | △ | Standard DRM interfaces and safe scanout references implemented; complete buffer-layout, synchronization and failure-path qualification remains. |
| fbcon, Wayland, GNOME, libMali output | Supported | △ | Console and desktop operation confirmed; full vendor/S7D format, color and output-profile coverage remains. |
| OSD1 primary, OSD2 cursor / overlay | Supported | △ | Cursor and overlay share OSD2. Additional formats, compression and complete composition qualification remain. |
| VD1 / VD2 video planes | Supported | △ | Two video planes with mutable order and alpha; four-input board subset exercised VD1 NV12 and VD2 NV21 with an opaque bottom input. Exact pixel/color and full format/combination qualification remain; VD2 + AFBC stays restricted. |
| Linear RGB scanout | 16/24/32-bit and additional formats | △ | RGB565, RGB888/BGR888 and 32-bit RGB/alpha variants. |
| Linear YUV scanout | Multiple YUV formats | △ | VD1/VD2 support NV12/NV21/UYVY/VYUY and packed VUY888 8-bit 4:4:4 at 1:1. VD2 VUY scaling additionally passed 126 UYVY/VUY equal-color CRC pairs, crop and geometry controls. Packed 4:2:2 chroma switches use live matrix/base updates. VD1 VUY scaling has source/build checks only; horizontal full-chroma phase, odd geometry, 10-bit and other formats remain. |
| OSD / video scaling | Hardware scalers | △ | Two OSD scalers, VD1 SAFA and VD2 classic PPS. VD2 VUY enlargement/reduction passed 254 board presentations, 112 geometry controls, 12 crop/pre-cut equalities, 36 vertical-chroma responses and 1:1/NV12 returns. Horizontal 8-tap and vertical 3/4-tap are implemented. VD1 VUY scaling remains a source/build-checked candidate; horizontal phase, full ratios, precision, filters and combinations remain unqualified. |
| Graphics AFBC scanout | Multiple surfaces / layouts | △ | One AFBC372 surface, modifier 0x0800000000000372. Mali AFBCD remains ABGR8888-only; optional GFCD primary also accepts opaque XBGR8888. Additional surfaces, formats and layouts remain. |
| Mali AFBCD backend | Display-side AFBC decoder | △ | Primary or OSD2 AFBC, local OSD scaling and restricted video mixing; additional formats and combinations remain. |
| Multiple simultaneous AFBC surfaces | Supported | ✗ | Current native limit is one compressed graphics surface. |
| GFCD AFBC backend | S7D AFBC/AFRC decoder; disabled in default C5 vendor DT | △ | Optional ABGR/XBGR AFBC372 primary, opaque enlargement up to 4× into FHD / 148.5MHz, and unscaled premultiplied ABGR. Actual XBGR buffers passed 120 swaps/reuses and X=0/255 opaque CRC comparisons under NONE/PREMULT; ABGR PREMULT repeatability passed separately, absolute color accuracy remains unproven. Static scaled primary + linear OSD2 updates reached 60.04fps; changing the scaled primary remains about 30fps. Crop, reduction, video mixing, simultaneous Mali AFBCD, remaining formats/surfaces and higher-mode qualification remain. |
| AFRC buffers and scanout | GFCD hardware path | ✗ | No native AFRC modifier or scanout support; producer/export support is unverified. |
| Amlogic compressed video FBC scanout | Supported | ✗ | Separate from graphics AFBC; native video planes use linear buffers. |
| Four-input composition and z-order | Two OSDs and two videos | △ | At 1024×600, 39 scenes / 156 board commits covered order, alpha, reflection and VD2 scaling with successful fences and stable CRCs. Exact pixels/colors remain unverified. Opaque bottom required; compression combinations remain restricted. |
| Video global alpha | VD1 / VD2 blend alpha | △ | Five alpha values per video plane (0..65535) completed board commits with stable scene CRCs. Exact blend values/colors remain unverified; four-input bottom remains opaque. |
| Plane reflection | RGB and video direction controls | △ | X/Y/XY on all four linear inputs have board subsets; packed video includes VUY888 crop/XY equal-color comparisons. Complete format/direction qualification and compressed reflection remain. |
| Quantitative bandwidth model | Shared memory and fetch paths | △ | Payload, conservative padded/AFBC fetch bounds, exact-rate average and active-window demand, including RDMA; visible in DRM state. No calibrated shared-DDR admission limit yet. |
| 10-bit RGB/YUV, additional YUV formats | Supported formats depend on the block | △ | Current SDR scanout includes 8-bit planar, packed 4:2:2 and packed 4:4:4 subsets; 10-bit and remaining layouts are not implemented. |
| Gamma / CTM / HDR / picture processing | S7D color, HDR and enhancement blocks | ✗ | Basic BT.601/709 limited/full YUV-to-RGB conversion only; advanced processing remains. |
| GE2D conversion, scaling, blending | Separate vendor accelerator | ✗ | Deferred; not part of the current DRM scanout path. |
| Progressive HDMI RGB8, up to 4K60 | Supported | △ | 1080p and UHD 4K30/60 output confirmed; full timing/sink compatibility and recovery qualification remains. |
| FHD high refresh / fractional timings | S7D vendor profile allows up to 120Hz at FHD and 60Hz at 4K | △ | Shared mode validation applies the S7D envelope, including fractional rates, before register planning. Full pipeline limits and board qualification remain. |
| EDID detailed timings / small panels | Vendor timing tables | △ | DRM EDID helpers and timing/PLL validation; VU7A+ 1024×600 and Vu5A 800×480 basic output confirmed. VU12 now uses EDID-selected 1920×720 / about 59.86Hz with active GNOME and CRC after normal reboot. No panel-specific DT or EDID override was required; complete panel/hotplug qualification remains. |
| HDMI / DVI mode selection | Supported | △ | DVI handling implemented; physical DVI-adapter qualification remains. |
| HPD, DDC, ELD, SCDC setup | Supported | △ | Vendor-aligned 50kHz DDC and bounded failed-EDID retry; repeated hotplug regression and continuous link-error monitoring remain. |
| HDMI YCbCr, deep color, range selection | Supported / sink-dependent | ✗ | Native TX is RGB 8-bit full range. |
| Interlaced output / pixel repetition | Supported | ✗ | Native output accepts progressive timings only. |
| HDR10 / HLG / dynamic HDR output | S7D HDR paths | ✗ | Requires the color pipeline, bit depth, metadata and TX packets together. |
| CEC | Supported | ✗ | No native CEC adapter connection. |
| VRR / QMS / QFT / ALLM / SBTM | Listed in the S7D datasheet; vendor execution scope varies | ✗ | Native standard properties and S7D timing/packet paths not connected. |
| VPP frame CRC | Vendor CRC support | △ | Standard DRM CRC interface; 39 composition/scaling scenes had four stable samples each, and GFCD scale/unscale preserved the 1:1 phase CRCs. This establishes repeatability, not exact pixel/color or HDMI-output correctness; complete output-profile qualification remains. |
| Sustained ≥60fps presentation | Mode-dependent; no equivalent vendor benchmark yet | △ | Single-surface and static scaled GFCD subsets reached about 60 flips/s. Fixed VD1/VD2 with changing linear OSD1/OSD2 now reach 60.03fps versus 30.02fps with matching phase CRCs. Replaced video, changing scaled GFCD, full composition, 4K and higher-refresh qualification remain. |
| Frame pacing | Compositor / GPU / display dependent | △ | Unchanged GFCD and unchanged video no longer defer eligible linear OSD flips. Replaced video keeps later-field retirement (about 33ms in current 60Hz address-flip tests); changed compressed buffers keep their guards. Full mixed-plane pacing remains. |
| HDMI stereo PCM | Supported | △ | ASoC / hdmi-codec / ELD; 2ch 16/24-bit, 44.1/48kHz. Earlier listening confirmed; further listening deferred. |
| Analog stereo output | Supported | △ | Minimal TDM C / T9015 path and DT overlay implemented; listening deferred. |
| External SPDIF output | Overlay-enabled | △ | Minimal FRDDR / SPDIF A path and DT overlay implemented; listening deferred. |
| Multichannel / high-rate / non-PCM HDMI audio | Supported formats depend on the sink | ✗ | Native HDMI audio currently exposes stereo LPCM only. |
| GPU rendering | Vendor Mali kbase CSF + libMali | △ | Existing kbase/libMali retained with standard resource consumers; full SoC/resource/interop qualification is not established. Panthor/Panfrost remains excluded. |
| GPU telemetry / desktop monitoring | Vendor kbase counters and devfreq | △ | Clock, busy/idle and shared-memory statistics exist; Resources integration and SoC thermal-to-hwmon exposure are deferred. No dedicated GPU temperature is exposed. |
| DDR metadata / desktop monitoring | Firmware and vendor DDR information | △ | Firmware-reported DDR frequency is available; memory type lacks a user-facing interface and Resources has no DMI data. Improvements deferred. |
| Clock / PLL / reset / PHY / PM ownership | Mix of providers and global helpers | △ | Native DT consumers/providers and firmware SCMI/SMC services. Core clock sampled once per atomic check; protected prepare revalidation retained. Eighteen driver DT bindings cover the current 20 compatibles and pass original board/analog/SPDIF configurations; full S7D resource/power qualification remains. |
| Display power-off / cold initialization | Vendor boot/power sequencing | ✗ | Shared VPU/HDMI domain remains on; retained firmware/SRAM state still required. |
| Screen blanking / suspend / error recovery | Vendor lifecycle paths | △ | Basic blanking and s2idle restoration exist. VD2 preparation failure was reproduced and fixed; subsequent scene restoration, buffer retirement and normal reboots passed. Full automatic recovery, HPD/PM races, power-loss recovery and deep sleep remain unqualified. |
| U-Boot logo → simpledrm → native KMS | Vendor logo / boot framebuffer path | ✗ | Continuous handoff and safe logo-memory return deferred. |
| Kernel video firmware loading | Vendor firmware / TEE service | △ | Native archive validation and existing TEE loading work for current codecs; complete S7D firmware/codec coverage remains. |
| Standard stateful V4L2 decoding | Vendor V4L2 plus vendor APIs | △ | H.264/HEVC/VP9/AV1 nodes, MMAP queues, PTS, drain/EOS and CAPTURE DMA-BUF export; API gaps remain below. |
| H.264 decoding | Up to 4K30, HP@L5.1 | △ | Limited 8-bit progressive Baseline/Main/High; representative 1080p replay verified, full coding-tool coverage incomplete. |
| HEVC decoding | Up to 4K60, Main10@L5.1 | △ | Experimental Main 8-bit 1080p subset; representative replay verified, tiles/multiple slices and Main10 absent. |
| VP9 decoding | Up to 4K60, Profile2 | △ | Experimental Profile 0 / 8-bit 1080p; hidden/reference replay and superframes implemented; scaling/DRC absent. |
| AV1 decoding | Up to 4K60, Main10@L5.1 | △ | Experimental Main 8-bit 1080p; representative replay verified; grain, super-resolution and DRC absent. |
| Full codec profiles / 10-bit / 4K coverage | S7D codec-specific limits | ✗ | Native codec names do not imply the complete vendor specification. |
| Seek / dynamic resolution / general-player compatibility | Vendor playback paths | △ | H.264/HEVC seek tests failed or stalled; limited DRC source paths unverified; VP9/AV1 OUTPUT-only seek absent. |
| DMA-BUF import / end-to-end zero-copy / concurrent decode | Vendor buffer and scheduling paths | ✗ | V4L2 MMAP and CAPTURE export available; copies and one active DOS lease remain. |
| MPEG1/2, MPEG4, VC1/WMV, AVS/AVS2, MJPEG/JPEG decode, MVC | S7D vendor decoder coverage | ✗ | Not ported to the native V4L2 path. |
| JPEG encoding | S7D HCODEC path | △ | Optional experimental V4L2 encoder implemented; actual JPEG output not board-verified. |
| H.264 encoding | Up to 1080p30 / 720p60 | ✗ | Native encoder not implemented. |
| amstream / amvideo / vendor media API compatibility | Supported | ✗ | Not reproduced; standard V4L2 takes priority, vendor API parity is not promised. |
