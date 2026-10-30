# HDMI sink model

Host tests compile the current S7D HDMI output, timing, DDC and SCDC code against
synthetic EDID/I2C fixtures. They require Python3, a host C compiler with UBSan,
`edid-decode`, and the complete kernel tree containing `common_drivers`.

```sh
make check KERNEL_SRC=/path/to/common O=/tmp/c5-hdmi-check
make guards KERNEL_SRC=/path/to/common
# CC="ccache gcc" and PYTHON=/path/to/python3 are supported.
```

The output directory must be outside the kernel source tree, including symlink
aliases. Each invocation creates a new run directory; `summary.json` identifies
the latest running, failed or passed attempt. Generated EDIDs, source excerpts,
patterns and result records stay in that output directory. `make fixtures`
creates only the synthetic EDIDs and low-bit-sensitive ABGR2101010 input pattern.

Coverage includes output/timing policy, EDID capability parsing, native DDC
faults, SCDC negotiation and path/result guards. The local DDC fixtures replace
an external development helper; actual production functions remain extracted
from `KERNEL_SRC` at execution time.

These tests access no board or display device and install no EDID. They do not
qualify physical HDMI transmission, HDR brightness, VDIN capture or 10-bit output
precision. Fixture capabilities do not enable unsupported driver functions.
