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

## Audience and goals

- Learn how Linux kernel display drivers and their hardware resources fit together.
- Use and improve ODROID-C5 display support through standard Linux interfaces.
- Explore the Amlogic S7D graphics, display and media architecture.

## Tested displays

- AlphaScan AOC 2269WM
- LG HDR 4K 27UK600
- Hardkernel ODROID VU7A+
- Hardkernel ODROID VU7+
- Hardkernel ODROID VU5A
- Hardkernel ODROID VU12

Automated validation also uses DRM writeback and headless KMS, with host-side virtual HDMI sink tests. Headless capture exercises the real S7D VPU without a connected monitor; virtual sink tests check negotiation and calculation paths. Neither verifies physical HDMI transmission or a monitor's rendering.

## Feature coverage

### Functionality in scope

The following vendor/S7D capabilities define the scope of this work. They are not all implemented in the replacement drivers; the table below records native support and remaining work.

- HDMI display output through DRM/KMS, with internal VPU capture through standard DRM writeback.
- H.264, HEVC, VP9 and AV1 decoding.
- MPEG-1/2, MPEG-4, VC-1/WMV, AVS/AVS2, MJPEG/JPEG decoding and MVC.
- H.264 and JPEG encoding.
- Graphics AFBC scanout through the Mali AFBCD and GFCD backends, and the separate Amlogic compressed-video FBC path.
- GE2D color conversion, scaling and alpha blending; integration is deferred.

### Experimental or incomplete capabilities

These capabilities are incomplete or lack full hardware qualification; some are not exposed by the native drivers yet. Limited use, source/build checks or a successful subset of tests do not establish complete support.

- 10-bit color and deep-color capture.
- HDR output and capture profiles.
- 4K and higher-resolution output across all plane and color configurations.
- Refresh rates above 60Hz and sustained presentation performance.
- Multichannel HDMI PCM audio.

### Out of scope

- CVBS output.
- HDMI video or audio input, including eARC reception.
- Unused board interfaces without an accessible connection on ODROID-C5.
- Native 12-bit writeback and end-to-end 12-bit precision qualification in the current phase; retained HDMI transport code is a separate capability.

### Implementation status

| Feature | Vendor / S7D | New driver | Scope / remaining work |
| --- | --- | :---: | --- |
| **Status — 2026-10-27** | S7D-specific implementation; not every feature is enabled on C5 | ✓ / △ / ✗ | ✓ Complete vendor/S7D feature coverage implemented and verified; △ partial, in progress, or not fully qualified; ✗ not implemented. A successful subset or source/build check alone does not earn ✓. |
| **Display implementation target** | All documented S7D display capabilities, including features disabled in the C5 vendor profile | △ | Source/calculation/build validation and selected board tests; complete vendor/S7D parity is still the target. Existing exclusions for protected/proprietary content and unused inputs remain. |
| **Latest verified baseline** | Current validation and recovery | △ | Committed RGB8 writeback passed exact-pixel capture through UHD/DCI 4K60 and FHD120. Native-timing RGB10 packing now preserves original pixel coordinates and all 1024 component levels. RGB10 targets also passed RGB8-reference captures through UHD 4K60/FHD120 and native CTM/gamma comparisons. HDR remains a source/build prototype. These results do not establish high-rate HDMI transmission, sustained performance or complete S7D coverage. |
| DRM atomic KMS, TEST_ONLY | Supported | △ | One CRTC. Earlier 41 board TEST_ONLY requests left reported state unchanged, including two expected rejections; all 39 scenes completed event/fence updates after the readback fix. Build 164 completed four full unchanged connector/scalar/disabled-plane AFBC replacements without ALLOW_MODESET and three connector-only TEST_ONLY state/clock/CRC invariance probes. Effective connector changes still require modeset permission. Internal mode flags and latency improvement were not measured; complete lifecycle and error-path qualification remains. |
| S7D timing and shared-resource admission | SoC limits and shared display blocks | △ | Common FHD120/UHD60 envelope and per-frame input, compression and scaler claims; display-fetch demand is modeled. Shared-DDR capacity qualification remains. |
| GEM, dumb buffers, DMA-BUF, fences | Supported | △ | Standard DRM interfaces and safe scanout references implemented; complete buffer-layout, synchronization and failure-path qualification remains. |
| fbcon, Wayland, GNOME, libMali output | Supported | △ | Console and desktop operation confirmed; full vendor/S7D format, color and output-profile coverage remains. |
| OSD1 primary, OSD2 cursor / overlay | Supported | △ | LINEAR primary supports integer crop, position, reflection and local scaling; build 158 passed a limited RGB8/partial-OSD2 subset. Build 159 passed positioned/cropped/scaled GFCD primary with partial LINEAR OSD2. Build 160 passed a Mali S1 compressed-OSD2 crop/scale subset with a fixed opaque LINEAR primary and restoration. Build 163/GFCD-off passed a grouped Mali S0 original-ROI crop, unity X/Y/XY and 1.5× XY scale/new-buffer subset with automated restoration. Equal-geometry buffer flips add no new scope writes; cursor/overlay share OSD2. Complete vendor/S7D combinations and recovery remain unqualified. |
| VD1 / VD2 video planes | Supported | △ | Two video planes with mutable order and alpha; four-input board subset exercised VD1 NV12 and VD2 NV21 with an opaque bottom input. Exact pixel/color and full format/combination qualification remain; VD2 + AFBC stays restricted. |
| Linear RGB scanout | 16/24/32-bit and additional formats | △ | RGB565, RGB888/BGR888, 32-bit RGB/alpha and linear ABGR2101010/XBGR2101010 on primary/OSD2. ABGR code-value/alpha and XBGR X=0..3 NONE/PREMULT subsets passed board comparisons. XBGR is derived from shared RGB packing and documented alpha replacement; the selected vendor does not advertise it directly. Complete format, scaling, precision and composition coverage remains. |
| Linear YUV scanout | Multiple YUV formats | △ | VD1/VD2 support NV12/NV21/UYVY/VYUY and packed VUY888 8-bit 4:4:4. NV16/NV61 full-height semiplanar 4:2:2 input passed bounded format/crop/reflection/2x-scale comparisons on both engines in build 170. Six NV16/NV61 pairs and four constant-chroma UYVY controls matched; rich-chroma output differs from packed UYVY under the vendor formatter policy, so exact filtering equivalence remains unqualified. VUY scaling subsets on both engines passed equivalent UYVY color CRCs, crop/geometry controls and relative horizontal-phase checks. Packed 4:2:2 chroma switches use live matrix/base updates. Complete chroma precision, odd geometry, 10-bit and other layouts remain unqualified or unimplemented. |
| OSD / video scaling | Hardware scalers | △ | Two OSD scalers, VD1 SAFA and VD2 classic PPS; VUY scaling/crop/chroma/return subsets passed earlier board checks. Build 163/GFCD-off matched Mali S0 512×288→768×432 XY scaling and same-layout/new-buffer flips to GPU-LINEAR references with automated restoration; CRC is diagnostic. The S7D wide-input path for OSD1/2 widths 1921–3840 is included in build 150 but has not been exercised with wide scaler input. ≤1920 plans/register writes remain unchanged. GFCD decoded ROI/FHD and 148.5MHz limits remain; full ratios, filter precision, 4K composition and performance remain unqualified. |
| Graphics AFBC scanout | Multiple surfaces / layouts | △ | AFBC372, modifier 0x0800000000000372. One GFCD primary and one Mali S1 surface may run concurrently; build 165 qualified a limited crop/scale/reflection/replacement and shared-FB subset. Mali AFBCD remains ABGR8888-only; GFCD primary also accepts opaque XBGR8888. Two Mali surfaces, dual GFCD, additional formats and layouts remain outside this supported pair. |
| Mali AFBCD backend | Display-side AFBC decoder | △ | Primary or OSD2 AFBC, local OSD scaling and restricted video mixing. Integer source cropping preserves the original framebuffer allocation/stride and source-coordinate ROI; stable-ROI address flips use the existing fast path. Build 160 exercised Mali S1 OSD2 full source, integer ROI and scaling. Build 162, with GFCD-on, added X/Y/XY and same-direction/new-buffer comparisons on original ROI, matching four GPU-LINEAR CRC pairs with user confirmation and restoration. CRC is diagnostic. Build 163/GFCD-off passed a grouped Mali S0 original-ROI X/Y/XY, 768×432 XY scale and same-scaled-XY/new-buffer subset with five stable/equal GPU-LINEAR comparisons and automated restoration; no new human visual confirmation is claimed. Build 165 additionally passed a concurrent GFCD-primary+single-Mali-S1 crop/scale/reflection/replacement and shared-FB subset with automatic restoration. Two Mali surfaces, broader formats and remaining reflection/scale/composition combinations remain unqualified. |
| Multiple simultaneous AFBC surfaces | Supported | △ | One independent GFCD primary+one Mali S1 compressed surface is supported concurrently. Build 165 passed independent/joint replacements, already-signaled same-buffer producer dependency and exact imported-FB sharing against GPU-LINEAR references. Pending GPU waits, two Mali surfaces, dual GFCD and remaining combinations are unqualified or unsupported. |
| GFCD AFBC backend | S7D AFBC/AFRC decoder; disabled in default C5 vendor DT | △ | ABGR/XBGR AFBC372 primary with integer ROI, opaque reduction up to 2× and enlargement up to 4× per axis; unscaled premultiplied ABGR. Build 162 exercised original-ROI X/Y/XY and same-direction/new-buffer flips at unchanged destination coordinates, matching four GPU-LINEAR CRC pairs with user confirmation and restoration; CRC is diagnostic. Build 159 earlier exercised real Mali ABGR 640×360 movement, ROI (32,24,512,288), 768×432 scaling and partial LINEAR OSD2. Build 150 exercised 3840×2160 and 4096×2160 allocations with centered 1920×720 ROI, independent LINEAR CRCs and old-buffer GPU redraws. Build 165 additionally passed concurrent GFCD primary+single Mali S1 with separate/joint buffer replacement and exact FB sharing, using seven relative CRC pairs and automatic restoration. These subsets do not prove full-4K decode or throughput. Decoded ROI/output stay FHD and ≤148.5MHz; full format/ROI/reflection/scale combinations, filter/PREMULT accuracy, video mixing, dual GFCD, additional Mali surfaces and remaining combinations remain unqualified or unimplemented. |
| AFRC buffers and scanout | GFCD hardware path | ✗ | No native AFRC modifier or scanout support; producer/export support is unverified. |
| Amlogic compressed video FBC scanout | Supported | ✗ | Separate from graphics AFBC; native video planes use linear buffers. |
| Four-input composition and z-order | Two OSDs and two videos | △ | At 1024×600, 39 scenes / 156 board commits covered order, alpha, reflection and VD2 scaling with successful fences and stable CRCs. Exact pixels/colors remain unverified. Opaque bottom required; compression combinations remain restricted. |
| Video global alpha | VD1 / VD2 blend alpha | △ | Five alpha values per video plane (0..65535) completed board commits with stable scene CRCs. Exact blend values/colors remain unverified; four-input bottom remains opaque. |
| Plane reflection | RGB and video direction controls | △ | X/Y/XY on all four linear inputs have board subsets; packed video includes VUY888 crop/XY equal-color comparisons. Committed build 162 passed eight stable/equal relative pairs for sequential Mali S1 OSD2 and GFCD primary X/Y/XY and same-direction/new-buffer flips, with unchanged ROI/destination coordinates, user confirmation and final restoration. CRC is diagnostic. Build 163/GFCD-off additionally passed automated Mali S0 unity X/Y/XY and 1.5× XY scale/new-buffer comparisons with restoration and no human visual claim. Full format/scale/composition coverage remains unqualified; this is not whole-S7D completion. |
| Quantitative bandwidth model | Shared memory and fetch paths | △ | Payload, conservative padded/AFBC fetch bounds, exact-rate average and active-window demand, including RDMA; visible in DRM state. No calibrated shared-DDR admission limit yet. |
| 10-bit RGB/YUV, additional YUV formats | Supported formats depend on the block | △ | Linear ABGR2101010/XBGR2101010 input supports primary/OSD2 subsets. Opaque primary XBGR2101010 now preserves original coordinates and all 1024 component levels in native-timing writeback; RGB8-to-RGB10 and rounded RGB10-to-RGB8 conversions were checked separately. Full alpha, scaling and composition coverage remains. HDMI 10/12-bit transport code does not establish effective wire precision; 10-bit YUV input and compressed RGB10 remain unsupported. Twelve-bit precision qualification is outside the current phase. |
| Gamma / CTM / HDR / picture processing | S7D color, HDR and enhancement blocks | △ | Standard CTM and a 65-entry GAMMA_LUT after CTM are implemented for RGB output (limited range is applied after the LUT) and 8-bit YCbCr444, with NULL bypass and stopped table updates. Build 194 brought GNOME up on a CTA limited-range mode with its gamma LUT and read back the range-folded table. Ordinary flips add no LUT writes, copies or waits. Native-panel writeback matched CTM and constant-gamma pixel references; normalized linear-LUT output differs from bypass, so exact knots, interpolation and precision remain unqualified. Gamma with 10/12-bit YCbCr444 and YCbCr422 (converted in POST2), degamma, HDR and full picture processing remain incomplete. |
| GE2D conversion, scaling, blending | Separate vendor accelerator | ✗ | Deferred; not part of the current DRM scanout path. |
| Progressive HDMI RGB8, up to 4K60 | Supported | △ | 1080p and UHD 4K30/60 output confirmed; full timing/sink compatibility and recovery qualification remains. |
| FHD high refresh / fractional timings | S7D vendor profile allows up to 120Hz at FHD and 60Hz at 4K | △ | Shared mode validation applies the S7D envelope, including fractional rates, before register planning. Full pipeline limits and board qualification remain. |
| EDID detailed timings / small panels | Vendor timing tables | △ | DRM EDID helpers and timing/PLL validation; VU7A+ 1024×600 and Vu5A 800×480 basic output confirmed. VU12 now uses EDID-selected 1920×720 / about 59.86Hz with active GNOME and CRC after normal reboot. No panel-specific DT or EDID override was required; complete panel/hotplug qualification remains. |
| HDMI / DVI mode selection | Supported | △ | DVI handling implemented; physical DVI-adapter qualification remains. |
| HPD, DDC, ELD, SCDC setup | Supported | △ | Vendor-aligned 50kHz DDC and bounded failed-EDID retry; repeated hotplug regression and continuous link-error monitoring remain. |
| HDMI YCbCr, deep color, range selection | Supported / sink-dependent | △ | Sink-aware RGB/YCbCr444 8/10/12 transport and one rounded YCbCr422 output in the fixed 12-bit container with eight effective bits are implemented. Build 190 selected that YCbCr422 output for maximum 12/11 requests under a temporary 444-removed genuine-capability fixture and read back the rounded formatter settings; normal Y444 preference remains. 4K60 RGB/YCbCr444 is limited to 8 bits by the 600MHz TMDS envelope. Visibility was confirmed, but physical low-bit precision/chroma phase, genuine 422-only sinks and PCM listening remain unqualified. Higher effective YCbCr422 precision awaits TX-mask and source-alignment evidence; YCbCr 4:2:0 8/10/12 is implemented for 4:2:0-only modes and full-rate-link fallback (build 195 matched formatter, FIFO, half-rate TX clock and depth registers on a 4:2:0 fixture; reception by a real 4:2:0 sink is unverified, and the 12-bit deep-color phase check is unreliable on RGB and 4:2:0); Broadcast RGB Auto/Full/Limited is implemented with RGB8 Limited conversion and CTM preservation; bounded state/lifetime and affine CRC comparisons passed. Exact transmitted code values, Limited 10/12 and other sinks remain unqualified. BT.2020 and complete qualification remain. |
| HDMI content type / AVI ITC-CN | Standard No Data/Graphics/Photo/Cinema/Game hints | △ | Standard content-type values build AVI ITC/CN through the existing stopped-modeset path; equal values add no packet writes, and DVI stores the property without AVI. Source/helper packing and focused build checks passed; build 157 passed a seven-present/nine-TEST_ONLY RGB8 scalar protocol and restoration subset. Property acceptance and helper packing do not prove actual wire bits, monitor picture processing or latency. Game remains separate from ALLM. Complete vendor/S7D coverage and verification remain. |
| Interlaced output / pixel repetition | Supported | ✗ | Native output accepts progressive timings only. |
| HDR10 / HLG / dynamic HDR output | S7D HDR paths | ✗ | Metadata and TX-packet prototypes have source/build checks only. A complete, qualified color pipeline, bit-depth contract, persistent HDR state and HDMI/capture output are not yet connected. |
| CEC | Supported | △ | Native standard adapter, dedicated CCF/reset/pinctrl resources and HDMI physical-address notifier implemented. Local API, clock lifetime and idle rebind verified on build 167. Real TX still fails with IRQ 0x11; GPIOH3 reads low and the controller/sink/cable cause is unresolved. Peer ACK/NACK/RX, HPD and s2idle qualification, RC input, runtime PM, deep standby/firmware wake and complete error recovery remain. |
| VRR / QMS / QFT / ALLM / SBTM | Listed in the S7D datasheet; vendor execution scope varies | ✗ | Native standard properties and S7D timing/packet paths not connected. |
| VPP frame CRC | Vendor CRC support | △ | Standard DRM CRC interface; 39 composition/scaling scenes had four stable samples each, and GFCD scale/unscale preserved the 1:1 phase CRCs. This establishes repeatability, not exact pixel/color or HDMI-output correctness; complete output-profile qualification remains. |
| VDIN1 / DRM writeback | S7D internal VPP loopback | △ | Standard connector exposes linear BGR888 and XBGR2101010 targets, fences and checked DMA retirement; HDMI-clone or real VPU/ENCP headless output. RGB8 exact pixels passed through UHD/DCI 4K60 and FHD120. RGB10 original-coordinate packing and all 1024 levels passed at native timing; RGB10 targets additionally passed UHD 4K60/FHD120 RGB8-reference captures and native CTM/gamma comparisons. Internal capture currently uses the RGB8/full-range carrier and does not qualify HDMI deep-color transmission. HDR, additional target formats and complete S7D coverage remain; native 12-bit capture is unsupported and excluded from the current phase. |
| Sustained ≥60fps presentation | Mode-dependent; no equivalent vendor benchmark yet | △ | Single-surface and static scaled GFCD subsets reached about 60 flips/s. Fixed VD1/VD2 with changing linear OSD1/OSD2 now reach 60.03fps versus 30.02fps with matching phase CRCs. Replaced video, changing scaled GFCD, full composition, 4K and higher-refresh qualification remain. |
| Frame pacing | Compositor / GPU / display dependent | △ | Unchanged GFCD and unchanged video no longer defer eligible linear OSD flips. Replaced video keeps later-field retirement (about 33ms in current 60Hz address-flip tests); changed compressed buffers keep their guards. Full mixed-plane pacing remains. |
| HDMI stereo PCM | Supported | △ | ASoC / hdmi-codec / ELD; 2ch 16/24-bit, 44.1/48kHz. Earlier listening confirmed; further listening deferred. |
| Analog stereo output | Supported | △ | Minimal TDM C / T9015 path and DT overlay implemented; listening deferred. |
| External SPDIF output | Overlay-enabled | △ | Minimal FRDDR / SPDIF A path and DT overlay implemented; listening deferred. |
| 40-pin I2S output | TDM A pads; not linked by the C5 vendor card | △ | FRDDR D / TDMOUT A lane 0 playback with MCLK pad 1 through the `i2s` overlay (pins 23 BCLK, 24 LRCLK, 21 data, 7 MCLK; exclusive with SPI0). Build 192 registered the link and transferred 48kHz/16-bit and 44.1kHz/24-bit stereo with 256/64/1 fs clocks, pad routing and clock release; no DAC was attached, so output and listening are unverified. Capture is not implemented. |
| Multichannel / high-rate / non-PCM HDMI audio | Supported formats depend on the sink | ✗ | Native HDMI audio currently exposes stereo LPCM only. |
| GPU rendering | Vendor Mali kbase CSF + libMali | △ | Existing kbase/libMali retained with standard resource consumers; full SoC/resource/interop qualification is not established. Panthor/Panfrost remains excluded. |
| GPU telemetry / desktop monitoring | Vendor kbase counters and devfreq | △ | Clock, busy/idle and shared-memory statistics exist; Resources integration and SoC thermal-to-hwmon exposure are deferred. No dedicated GPU temperature is exposed. |
| DDR metadata / desktop monitoring | Firmware and vendor DDR information | △ | Firmware-reported DDR frequency is available; memory type lacks a user-facing interface and Resources has no DMI data. Improvements deferred. |
| Clock / PLL / reset / PHY / PM ownership | Mix of providers and global helpers | △ | Physical shared HDMI PLL, serializer and pixel clocks use matched VPU/PHY/DT consumers. Build 149 first-modeset EBUSY was resolved by selected native boot ownership in build 150; build 153 retained active rates/references during 8-bit fallback checks. Its DTB/initrd/modules are unchanged from the four-property build-150 qualification set. Unsupported retained partial PHY states are rejected; dormant physical gates and full PM remain incomplete. |
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

## Differences from the vendor drivers

The native drivers follow the selected S7D vendor implementation unless the hardware documentation or board validation supports a different choice. The table records intentional configuration and structural differences; missing native features are listed in the status table above.

| Area | Vendor (selected S7D / C5 configuration) | Native drivers | Reason |
| --- | --- | --- | --- |
| Display stack | `aml_drm` and `aml_media` share global state; VOUT/HDMI settings come from `outputmode`, `attr` strings and vendor properties | One DRM device, HDMI bridge, generic PHY, CCF, reset and genpd consumers | Standard KMS ownership and explicit DT resources |
| Mode selection | Vendor timing table and VIC whitelist; ODROID custom VICs; DT `max_refresh_rate=120` | DRM EDID timings checked against timing/PLL/TMDS limits; same 4K60 / FHD120 envelope | Monitor timings without name tables; envelope kept until higher rates are qualified |
| HDMI link | TMDS only; FRL and DSC are not enabled on S7D | TMDS only, up to 600MHz; FRL and DSC are not planned | S7D PHY and datasheet provide no FRL path |
| Deep color selection | `attr` depth request | Standard `max bpc` and `Colorspace`; 12 → 10 → 8 within sink and TMDS limits | Standard properties and automatic fallback |
| YCbCr422 | 12-bit attribute only (36B); formatter rounding and dither off; TX keeps 8 bits | One 8-bit-effective output in the same 12-bit container with formatter rounding on | Identical wire format; rounding avoids truncation bias |
| 8-bit dither | `VPU_HDMI_DITH_CNTL` enables 10→8 dither for 24-bit output | Rounding only, no dither | Not yet evaluated; recorded difference |
| Gamma | S7D DT disables gamma (`gamma_en=0`) | Standard 65-entry `GAMMA_LUT` after CTM for RGB and 8-bit YCbCr444; with a LUT, limited RGB range is folded into the LUT outputs instead of POST2 | Board-verified addition; keeps CTM → gamma → range order |
| Color transforms and range | Vendor PQ / amvecm controls | Standard CTM and `Broadcast RGB` through POST2 | Standard DRM color properties |
| GFCD | Present but disabled in the C5 vendor DT (`gfcd_enable=0`) | GFCD AFBC primary enabled with a Mali AFBCD overlay | Board-verified addition |
| VDIN1 capture | Vendor capture interfaces, 8-bit | Standard DRM writeback, BGR888 and XBGR2101010 | Standard UAPI; native 10-bit storage verified |
| CEC | `aocec` vendor driver | Standard CEC adapter with HDMI physical-address notifier | Standard CEC framework |
| HDMI audio | Auge card: TDM B (`i2s2hdmi`, 2×32-bit slots) and SPDIF B to HDMI | FRDDR A → TDMOUT B → HDMI, stereo 16/24-bit, 44.1/48kHz | Minimal standard ASoC path; multichannel and passthrough pending |
| Analog and SPDIF output | TDM C to the internal codec; SPDIF A | FRDDR B → TDMOUT C → T9015; FRDDR C → SPDIF A | Separate FIFOs per output |
| 40-pin I2S | TDM A data out on lane 1 and in on lane 0 through vendor audio pinctrl; MCLK pad clock | FRDDR D → TDMOUT A lane 0 on `TDM_D0`; pads routed by the PCM driver; MCLK pad 1 owned by CCF; playback only | No vendor audio pinctrl in the native stack |
| Boot display handoff | U-Boot logo reused by vendor DRM | Not implemented; Linux performs its own modeset | Deferred to the final stage |
