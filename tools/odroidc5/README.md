# ODROID-C5 display test tools

| Directory | Purpose |
|---|---|
| `writeback` | Standard DRM writeback captures, RGB8/RGB10 CTM/gamma checks, target replacement and runtime mode transitions. |
| `packing` | Raw RGB8/RGB10 source-to-target packing diagnostics and CPU fixture generation. |
| `hdmi-sink` | Host-only policy, EDID, DDC and SCDC tests using current production C functions. |
| `afbc-engine` | Host-only Mali AFBCD plan and decoder state machine checks against a fake register file, and GFCD AFRC size and buffer rules. |
| `common` | Shared standard-DRM UAPI helpers. |

Build inside the kernel checkout, or select another kernel root explicitly:

```sh
make KERNEL_SRC=/path/to/common CROSS_COMPILE=aarch64-linux-gnu- O=/tmp/c5-tools
make KERNEL_SRC=/path/to/common O=/tmp/c5-tools check
```

`KERNEL_SRC` defaults to the kernel parent of this installed directory. `CC`,
`CROSS_CC`, `CROSS_COMPILE`, `PYTHON` and `O` are configurable. Build outputs,
fixtures and measurements belong outside the source tree; the default is
`/tmp/odroidc5-tools`. The C programs use project DRM UAPI headers with the
selected libc's Linux/asm user headers, without libdrm or vendor libraries.

`make check` uses only the host: raster/packing checks and an actual-C HDMI
model. It requires a C compiler with UBSan, Python3 and `edid-decode`.
`make fixtures` generates synthetic EDIDs and a low-bit-sensitive input pattern.
Individual directories describe board use and additional options.

Board programs require independently verified boot and kernel-notes pins and
an owned DRM master after stopping the compositor. A pending DMA job or failed
restoration retains resources for controlled recovery. Review that state
before terminating or retrying a process.

Writeback exercises the real VPU; host sink fixtures exercise source/model
behavior. Neither establishes physical HDMI quality, optical HDR, sustained
mode performance or normalized 10-bit precision. Advertised fixture/mode
capabilities do not add unimplemented driver support.
