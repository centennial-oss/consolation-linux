# Developer Setup

Consolation for Linux is a Qt 6/C++20 application. Development builds require Qt Base, Qt OpenGL Widgets, Qt SVG, PipeWire, CMake, and a C++ compiler. The video capture path talks directly to V4L2 with Linux ioctls; audio capture/playback uses PipeWire.

## Bootstrap Scripts

Use these helper scripts to install distro-specific dependencies from this document.
For CI/CD packaging builds, prefer version-pinned scripts:

```sh
scripts/install-deps-fedora-42.sh
scripts/install-deps-fedora-44.sh
scripts/install-deps-ubuntu-24-04.sh
scripts/install-deps-ubuntu-26-04.sh
scripts/install-deps-rpi-os-trixie.sh
```

To include packaging dependencies (`rpm-build` or `dpkg-dev`), pass `--with-packaging`:

```sh
scripts/install-deps-fedora-44.sh --with-packaging
```

## Fedora / Asahi Linux

Install the dependencies needed to configure, build, and test the app:

```sh
sudo dnf install \
  cmake \
  gcc-c++ \
  libjpeg-turbo-devel \
  ninja-build \
  pipewire-devel \
  qt6-qtbase-devel \
  qt6-qtsvg-devel
```

To build Fedora RPM packages with `cpack -G RPM`, also install:

```sh
sudo dnf install rpm-build
```

Configure and build with Ninja:

```sh
cmake --preset fedora-44
cmake --build --preset fedora-44
ctest --preset fedora-44
```

Fedora 44 ships a newer Qt than Ubuntu 24.04. Use the `fedora-44` preset for local Fedora builds so generated files stay in `build-fedora-44`.

Qt OpenGL Widgets is provided by `qt6-qtbase-devel` on Fedora, so no separate OpenGL Widgets development package is needed.

During audio source matching work, set `CONSOLATION_PIPEWIRE_AUDIO_SOURCE` to a PipeWire source node name or object serial to force the capture stream to a specific audio source.

## Ubuntu 24.04

Install the dependencies needed to configure, build, and test the app:

```sh
sudo apt update
sudo apt install \
  build-essential \
  cmake \
  libpipewire-0.3-dev \
  ninja-build \
  qt6-base-dev \
  qt6-base-dev-tools \
  libqt6svg6-dev
```

To build Ubuntu `.deb` packages with `cpack -G DEB`, also install:

```sh
sudo apt install dpkg-dev
```

Configure and build with Ninja:

```sh
cmake --preset ubuntu-2404
cmake --build --preset ubuntu-2404
ctest --preset ubuntu-2404
```

Ubuntu 24.04 ships Qt 6.4.2. Build release binaries for Ubuntu 24.04 inside an Ubuntu 24.04 environment so the executable does not require newer Qt symbol versions from Fedora or a local Qt install.

Qt OpenGL Widgets is provided by `qt6-base-dev` on Ubuntu, so no separate OpenGL Widgets development package is needed.

## Raspberry Pi OS Trixie

Raspberry Pi OS Trixie is based on Debian 13 Trixie. Debian Trixie ships `qt6-base-dev` 6.8.2, which is compatible with Consolation's Qt 6.4 minimum.

Install the dependencies needed to configure, build, and test the app:

```sh
sudo apt update
sudo apt install \
  build-essential \
  cmake \
  libpipewire-0.3-dev \
  ninja-build \
  qt6-base-dev \
  qt6-base-dev-tools \
  libqt6svg6-dev
```

To build Raspberry Pi OS/Debian `.deb` packages with `cpack -G DEB`, also install:

```sh
sudo apt install dpkg-dev
```

Configure and build with Ninja:

```sh
cmake --preset rpi-os-trixie
cmake --build --preset rpi-os-trixie
ctest --preset rpi-os-trixie
```

Qt OpenGL Widgets is provided by `qt6-base-dev` on Debian/Raspberry Pi OS, so no separate OpenGL Widgets development package is needed.

ALSA and system integration work may use these additional development packages:

```sh
sudo dnf install \
  alsa-lib-devel \
  systemd-devel
```

### Direct V4L2 Capture

Use `ffmpeg` as an independent V4L2 probe when comparing app behavior against another V4L2 client:

```sh
ffmpeg -hide_banner -f v4l2 -i /dev/videoN -frames:v 1 -f null -
```

If Consolation and `ffmpeg` differ for the same device and mode, compare the V4L2 ioctl sequence and accepted format/frame interval.
