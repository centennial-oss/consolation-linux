# Developer Setup

Consolation for Linux is a Qt 6/C++20 application. Development builds require Qt Base, CMake, a C++ compiler, and libv4l2 for the capture path.

## Fedora / Asahi Linux

Install the dependencies needed to configure, build, and test the app:

```sh
sudo dnf install \
  cmake \
  gcc-c++ \
  libv4l-devel \
  ninja-build \
  qt6-qtbase-devel
```

Configure and build with Ninja:

```sh
cmake --preset fedora-44
cmake --build --preset fedora-44
ctest --preset fedora-44
```

Fedora 44 ships a newer Qt than Ubuntu 24.04. Use the `fedora-44` preset for local Fedora builds so generated files stay in `build-fedora-44`.

## Ubuntu 24.04

Install the dependencies needed to configure, build, and test the app:

```sh
sudo apt update
sudo apt install \
  build-essential \
  cmake \
  libv4l-dev \
  ninja-build \
  qt6-base-dev \
  qt6-base-dev-tools
```

Configure and build with Ninja:

```sh
cmake --preset ubuntu-2404
cmake --build --preset ubuntu-2404
ctest --preset ubuntu-2404
```

Ubuntu 24.04 ships Qt 6.4.2. Build release binaries for Ubuntu 24.04 inside an Ubuntu 24.04 environment so the executable does not require newer Qt symbol versions from Fedora or a local Qt install.

## Raspberry Pi OS Trixie

Raspberry Pi OS Trixie is based on Debian 13 Trixie. Debian Trixie ships `qt6-base-dev` 6.8.2, which is compatible with Consolation's Qt 6.4 minimum.

Install the dependencies needed to configure, build, and test the app:

```sh
sudo apt update
sudo apt install \
  build-essential \
  cmake \
  libv4l-dev \
  ninja-build \
  qt6-base-dev \
  qt6-base-dev-tools
```

Configure and build with Ninja:

```sh
cmake --preset rpi-os-trixie
cmake --build --preset rpi-os-trixie
ctest --preset rpi-os-trixie
```

ALSA and MJPEG decoder work use these additional development packages:

```sh
sudo dnf install \
  alsa-lib-devel \
  libjpeg-turbo-devel \
  systemd-devel
```

### Direct V4L2 Capture

Use `ffmpeg` as an independent V4L2 probe when comparing app behavior against another V4L2 client:

```sh
ffmpeg -hide_banner -f v4l2 -i /dev/videoN -frames:v 1 -f null -
```

If Consolation and `ffmpeg` differ for the same device and mode, compare the V4L2 ioctl sequence and accepted format/frame interval.
