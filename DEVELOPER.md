# Developer Setup

Consolation for Linux is a Qt 6/C++20 application. The current skeleton only needs Qt Base, CMake, and a C++ compiler. The real capture and audio backend will add V4L2, PipeWire or ALSA, and MJPEG decode dependencies.

## Fedora / Asahi Linux

Install the dependencies needed to configure, build, and test the current Qt skeleton:

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

When the real capture and audio backend work begins, expect to install these additional development packages:

```sh
sudo dnf install \
  libv4l-devel \
  pipewire-devel \
  alsa-lib-devel \
  libjpeg-turbo-devel \
  systemd-devel
```
