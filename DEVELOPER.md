# Developer Setup

Consolation for Linux is a Qt 6/C++20 application. Development builds require Qt Base, Qt OpenGL Widgets, Qt SVG, PipeWire, CMake, and a C++ compiler. The video capture path talks directly to V4L2 with Linux ioctls; audio capture/playback uses PipeWire.

## Bootstrap Scripts

Use these helper scripts to install distro-specific dependencies from this document.

```sh
scripts/install-deps-fedora-42.sh
scripts/install-deps-fedora-44.sh
scripts/install-deps-ubuntu-24.sh
scripts/install-deps-ubuntu-26.sh
scripts/install-deps-rpi-os-trixie.sh
```

To include packaging dependencies (`rpm-build` or `dpkg-dev`), pass `--with-packaging`:

```sh
scripts/install-deps-fedora-44.sh --with-packaging
```

Use the Makefile targets to build specific versions, or `build-linux` to build locally.
