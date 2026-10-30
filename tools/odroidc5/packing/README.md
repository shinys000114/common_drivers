# Writeback source/target packing diagnostics

`packing-matrix` records unmodified writeback payloads for each selected source
and target format. It checks fences, source lifetime, guards, padding and
restoration. A successful collection does not qualify component packing,
low-bit precision, HDR or the HDMI wire format.

Build against this kernel's DRM UAPI and the compiler's exported Linux/asm
headers; no libdrm is required:

```sh
make KERNEL_SRC=/path/to/common O=/tmp/c5-packing-build
make KERNEL_SRC=/path/to/common O=/tmp/c5-packing-build check
```

`CC`, `CROSS_COMPILE`, `CROSS_CC`, `PYTHON`, `KERNEL_SRC` and `O` are configurable. The default output is `/tmp/odroidc5-packing`.
The default cross prefix is `aarch64-linux-gnu-`. The Makefile creates an
output-only `include/drm` symlink to the selected kernel DRM UAPI; other Linux
and asm headers come from the host or target libc toolchain. `make host` builds only host
programs. `make sanitize` runs the host selftest under ASan/UBSan. Build outputs
and generated captures should remain outside this source directory.

Before a board run, stop the compositor and verify that the original scanout is
fbcon-owned or inactive. Supply the independently checked boot ID and kernel
notes hash. Run one of the four source/target combinations per invocation:

```sh
sudo /tmp/c5-packing-build/packing-matrix-aarch64 --run \
  --source rgb10 --target rgb10 --captures 2 --route headless \
  --expected-boot UUID --expected-notes SHA256 --output /var/tmp/new-capture
```

| Choice | Source | Target requested through the DRM UAPI |
|---|---|---|
| `rgb8` | LINEAR XRGB8888, four bytes/pixel | LINEAR BGR888, three bytes/pixel |
| `rgb10` | LINEAR XBGR2101010, four bytes/pixel | LINEAR XBGR2101010, four bytes/pixel |

A target must be advertised and admitted by the running driver. A format name
alone does not establish the hardware's byte layout or precision. The fixed
mode is 1920x720 at111750kHz (H2016/2208/2496, V723/733/748, positive sync).
Headless detaches HDMI; clone requires a real sink advertising this timing.
CTM and gamma are NULL, other planes are disabled, and physical output requests
RGB8/Default/Automatic independently of source/target depth.

Source and target objects include guard rows. The raw files include active
rows and reported row padding, with guard rows excluded. `RAW` records the
actual pitch, fourcc, byte count, offset and source/target hash. Sources remain
immutable until all capture and restore operations retire. An unretired job
or failed restore enters `RECOVERY_HOLD`; inspect and recover it before releasing
or killing the process.

`fixture-reference` writes the actual C source rasters into a new directory.
`check.py` compares every generated8-bit and10-bit pixel with an independent
CPU reference. Neither opens a DRM device.

`fixture10.py` generates or analyzes a four-byte10-bit fixture using explicit
component-layout hypotheses. Its default `--pattern matrix` matches this tool,
including the final80 parity rows. `--pattern legacy` selects the earlier
all-coordinate lower region. The analyzer compares to a10-bit source formula;
it does not infer cross-depth normalization for an8-bit source. It never chooses
a best-fit format or claims capture completion or precision:

```sh
python3 fixture10.py --make-source /tmp/new-fixture
python3 fixture10.py --analyze /var/tmp/new-capture/headless-0.raw \
  --pitch 7680 --pattern matrix --output /tmp/new-analysis.json
```

`analyze-matrix.py` checks all four source/target combinations together. It
requires Python3 and NumPy, two captures per combination, and a common boot ID
and kernel-notes hash. Preserve each invocation's stdout as
`rgb8-rgb8.log`, `rgb8-rgb10.log`, `rgb10-rgb8.log` or `rgb10-rgb10.log`,
with its output directory of the same name alongside that log. Each invocation
must use `--captures 2`; the directory layout is independent of its location.
The analyzer checks recorded raw checksums and the source fixture, original
pixel coordinates, channel packing, conversion rules and the low-bit patterns:

```sh
python3 analyze-matrix.py --capture-root /var/tmp/packing-matrix \
  --output /tmp/new-matrix-analysis.json
```

Exit status 0 means the required references and capture protocol passed, 1 means
a valid collection failed them, and 2 means an input/dependency error. Results
apply to that recorded mode, source and target; they do not establish optical
precision, HDMI wire depth, HDR or arbitrary composition. Raw files are never
rewritten or corrected to make a comparison pass.
