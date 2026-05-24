# Developer Setup

Consolation for Linux is a Qt 6/C++20 application. Development builds require Qt Base, CMake, a C++ compiler, and PipeWire development headers for the primary Linux capture path. Direct V4L2 remains the fallback path.

## Fedora / Asahi Linux

Install the dependencies needed to configure, build, and test the app:

```sh
sudo dnf install \
  cmake \
  gcc-c++ \
  ninja-build \
  pipewire-devel \
  qt6-qtbase-devel
```

Configure and build with Ninja:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Direct V4L2 fallback, ALSA fallback, and MJPEG work use these additional development packages:

```sh
sudo dnf install \
  libv4l-devel \
  alsa-lib-devel \
  libjpeg-turbo-devel \
  systemd-devel
```

### Direct V4L2 Capture And WirePlumber

On Fedora desktops, WirePlumber may register UVC capture cards through the PipeWire/libcamera camera stack. If direct V4L2 capture reports `Device or resource busy`, verify whether the desktop camera services are holding the device:

```sh
ffmpeg -hide_banner -f v4l2 -i /dev/videoN -frames:v 1 -f null -
```

For local development only, you can temporarily stop the user camera/audio services, test direct V4L2 capture, then start them again:

```sh
systemctl --user stop xdg-desktop-portal xdg-desktop-portal-kde xdg-desktop-portal-gtk wireplumber pipewire-pulse pipewire

# Run Consolation or an ffmpeg V4L2 probe here.

systemctl --user start pipewire pipewire-pulse wireplumber xdg-desktop-portal
```

This is a development workaround, not the intended end-user experience. The production app needs either robust direct V4L2 ownership handling or a PipeWire camera path for systems where the desktop camera stack owns UVC devices.
