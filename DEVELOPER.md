# Developer Setup

Consolation for Linux is a Qt 6/C++20 application. Development builds require Qt Base, CMake, a C++ compiler, and Linux V4L2 headers for the capture path.

## Fedora / Asahi Linux

Install the dependencies needed to configure, build, and test the app:

```sh
sudo dnf install \
  cmake \
  gcc-c++ \
  ninja-build \
  qt6-qtbase-devel
```

Configure and build with Ninja:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

ALSA and MJPEG work use these additional development packages:

```sh
sudo dnf install \
  libv4l-devel \
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
