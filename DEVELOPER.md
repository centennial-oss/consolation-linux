# Developer Setup

Consolation for Linux is a Qt 6/C++20 application. Development builds require Qt Base, Qt OpenGL Widgets, Qt SVG, PipeWire, libjpeg-turbo, CMake, and a C++ compiler. The video capture path talks directly to V4L2 with Linux ioctls; audio capture/playback uses PipeWire.

## Bootstrap Scripts

Use these helper scripts to install distro-specific **build** dependencies from this document.

```sh
scripts/install-deps-fedora-42.sh
scripts/install-deps-fedora-43.sh
scripts/install-deps-fedora-44.sh
scripts/install-deps-ubuntu-22.sh
scripts/install-deps-ubuntu-24.sh
scripts/install-deps-ubuntu-26.sh
scripts/install-deps-rpi-os-trixie.sh
```

To include packaging dependencies (`rpm-build` or `dpkg-dev`), pass `--with-packaging`:

```sh
scripts/install-deps-fedora-44.sh --with-packaging
```

Use the Makefile targets to build specific versions, or `build-linux` to build locally.

## Build Dependencies (all platforms)

| Package | Purpose |
|---------|---------|
| CMake ≥ 3.22 | Build system |
| C++20 compiler | `g++` / `clang++` |
| Qt 6.4+ dev | UI, OpenGL widgets, SVG |
| PipeWire dev | Audio |
| libjpeg-turbo dev | Software MJPEG decode (libyuv) |
| Mesa EGL/GL dev | DMA-BUF OpenGL import |
| pkg-config | CMake discovery |

## Optional: Hardware MJPEG Decode (build-time)

Hardware MJPEG is **optional at build time**. If `libva` is not found, the app still builds and uses software decode only.

| Package | Fedora | Ubuntu / RPi OS | Purpose |
|---------|--------|-----------------|---------|
| libva + libva-drm | `libva-devel` | `libva-dev` | VA-API MJPEG on Intel/AMD (compile-time) |

V4L2 M2M JPEG decode uses only kernel headers and needs no extra dev package beyond a normal Linux build environment.

## Runtime: Hardware MJPEG (optional, end users)

Installed binaries do **not** require hardware decode. When present, MJPEG streams may use:

| Platform | Typical runtime packages | Verify |
|----------|-------------------------|--------|
| **Intel** | `intel-media-driver`, `mesa-va-drivers`, `libva` | `vainfo \| grep -i jpeg` → `VAProfileJPEGBaseline` + `VAEntrypointVLD` |
| **AMD** | `mesa-va-drivers`, `libva` | same `vainfo` check (`radeonsi` driver) |
| **ARM / RPi 4** | kernel codec modules (e.g. `bcm2835-codec`), often preinstalled on Raspberry Pi OS | `v4l2-ctl --list-devices` and look for an M2M **decoder** with MJPEG on OUTPUT |
| **Raspberry Pi 5** | — | No hardware MJPEG decoder; software path only |

Diagnostic tools (optional): `libva-utils` (`vainfo`), `v4l-utils` (`v4l2-ctl`).

## MJPEG Decode Paths (in-app)

When the capture format is MJPEG/JPEG, `MjpegDecoder` probes once per session:

1. **VA-API** (Intel/AMD) — if built with `CONSOLATION_HAVE_LIBVA` and `vainfo` would show JPEG decode
2. **V4L2 M2M** — separate `/dev/video*` codec node (common on ARM/RPi 4)
3. **libyuv + libjpeg-turbo** — CPU (always available when libjpeg is linked)
4. **Qt** — last-resort loader

On **x86_64**, VA-API is tried before V4L2 M2M. On **ARM**, V4L2 M2M is tried first.

With debug stats enabled in the UI, the overlay shows **HWJD** when hardware MJPEG decode was used in the current telemetry window.

### VA-API performance flags (`MjpegDecoderVaapi.cpp`)

Re-enable optimizations **one at a time** when chasing quality or latency regressions:

| Flag | Effect |
|------|--------|
| `kOptPersistentExportImage` | Reuse `VAImage` for `vaGetImage` (no per-frame create/destroy) |
| `kOptReuseVaTableBuffers` | Reuse picture/IQ/Huffman/slice-param VA buffers |
| `kOptFastSosHeaderParse` | SOS-only scan when JPEG dimensions unchanged |

**VA-API MJPEG DMA-BUF:** when GPU display is enabled and `vainfo` reports JPEG + DRM PRIME export, MJPEG is decoded on the GPU and exported via `vaExportSurfaceHandle` for display through `Nv12DmaBufGl` (no CPU `NV12ToARGB`). Session log: `VA-API MJPEG DMA-BUF display enabled`.

**Temporary MJPEG perf logging (stderr):** run with `CONSOLATION_MJPEG_PERF=1` to print per-stage VA-API timings and end-to-end session decode ms every 30 frames (not shown in the UI overlay).

## Per-Distro Notes

### Fedora 42–44

```sh
scripts/install-deps-fedora-44.sh
```

Adds `libva-devel` for VA-API. For runtime HW decode on Intel machines, install `intel-media-driver` and `mesa-va-drivers` (often already present on desktop installs).

### Ubuntu 22.04 / 24.04 / 26.04

```sh
scripts/install-deps-ubuntu-24.sh   # or -22 / -26
```

Adds `libva-dev`. Intel systems may also need `intel-media-va-driver` (or non-free variant) at runtime.

### Raspberry Pi OS (Trixie)

```sh
scripts/install-deps-rpi-os-trixie.sh
```

Adds `libva-dev` (harmless on Pi) and `v4l-utils` for inspecting M2M codec nodes. Pi 4 may use `bcm2835-codec` for hardware MJPEG; Pi 5 uses software decode for MJPEG.

## Building

```sh
make build
```

CMake defines `CONSOLATION_HAVE_LIBVA` when `pkg-config` finds `libva` and `libva-drm`.
