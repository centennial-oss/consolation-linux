# Consolation Linux TODO

This is the working build plan for getting from the current empty Linux repo to a usable Consolation build. The intended implementation order is to make the complete Qt UX work first against mock capture devices and mock playback, then replace the mock backend with real V4L2 video and PipeWire/ALSA audio once the app behavior is solid.

## 0. Project Baseline

- [ ] Confirm target baseline Linux distributions and minimum versions for Ubuntu, Fedora, Raspberry Pi OS, X11, and Wayland.
- [ ] Choose the initial Qt version target, likely Qt 6, and document required development packages.
- [ ] Choose the initial build system shape for C++, likely CMake first so IDEs and Linux packaging tools work cleanly.
- [ ] Define the top-level source layout:
  - [ ] `src/app` for application startup and main window wiring.
  - [ ] `src/ui` for Qt widgets, sheets, models, and view state.
  - [ ] `src/capture` for capture interfaces and backends.
  - [ ] `src/playback` for rendering, audio playback, timing, and controls.
  - [ ] `src/settings` for persisted preferences.
  - [ ] `src/platform/linux` for V4L2, PipeWire, ALSA, sysfs, and Linux-specific helpers.
  - [ ] `assets` for app icon and app metadata assets.
  - [ ] `tests` for focused unit and integration tests.
- [x] Copy or adapt shared project docs from the reference ports:
  - [x] `LICENSE`
  - [x] `NOTICE`
  - [x] `PRIVACY.md`
  - [x] `TRADEMARKS.md`
  - [x] Initial `README.md`

## 1. Buildable Skeleton

- [ ] Add a minimal C++ Qt application that opens one main window.
- [ ] Add CMake configuration for a debug build.
- [ ] Add app metadata constants for name, bundle/app ID equivalent, version, commit, and build date.
- [ ] Add a tiny settings wrapper around `QSettings`.
- [ ] Add a placeholder app icon and wire it into the Qt window.
- [ ] Add a smoke test or lightweight test executable that can run in CI or from the command line.
- [ ] Verify the app builds and launches on at least one Linux desktop.

## 2. Capture Backend Interfaces

- [ ] Define capture-domain models independent of the real Linux backend:
  - [ ] Capture device identity, display name, stable ID, USB path hints, vendor/product ID, and serial when available.
  - [ ] Resolution, frame rate, and pixel format grouped as RFF.
  - [ ] Video frame metadata, including dimensions, pixel format, capture timestamp, and storage type.
  - [ ] `FrameStorage` with `DmaBuf` and `CpuDecodedJpeg`.
  - [ ] Audio packet metadata matching the requested `AudioPacket` shape.
- [ ] Define abstract interfaces for:
  - [ ] Device discovery.
  - [ ] Device format enumeration.
  - [ ] Playback session start/stop.
  - [ ] Video frame delivery.
  - [ ] Audio packet delivery.
  - [ ] Device hotplug notifications.
- [ ] Add dependency injection or factory wiring so the app can switch between mock and real Linux backends without touching UX code.

## 3. Mock Device Detection And Playback

- [ ] Implement mock device discovery with realistic UVC capture cards.
- [ ] Implement mock format enumeration with common modes:
  - [ ] 1920x1080 at 60/30 FPS.
  - [ ] 1280x720 at 60/30 FPS.
  - [ ] 720x480 and 640x480 fallback modes.
  - [ ] Pixel formats including NV12, YUYV/YUY2, MJPEG, and at least one lower-priority "other" format.
- [ ] Implement last-selected RFF lookup per mock device.
- [ ] Implement default RFF selection:
  - [ ] Prefer 1080p 60p when available.
  - [ ] Prefer pixel formats in this order: NV12, YUY2/YUYV, MJPEG, other.
- [ ] Implement mock playback that emits a stable synthetic video image at the selected resolution and frame rate.
- [ ] Include visual motion or frame counters in mock playback so frame updates are obvious.
- [ ] Implement mock low-frame-rate behavior for testing the warning UI.
- [ ] Implement mock audio level or silent audio plumbing enough for the volume UI and future audio backend.

## 4. Complete Stopped-State UX

- [ ] Build the main window stopped state with the startup form.
- [ ] Add device scan and refresh behavior.
- [ ] Add the device dropdown.
- [ ] Add the RFF selector as resolution menu, frame-rate submenu, and pixel-format submenu.
- [ ] Disable the Play button until a complete RFF is selected.
- [ ] Style the Play button as a triangle-only button inside a circular hit box.
- [ ] Persist the user's last-selected RFF per device.
- [ ] Restore the last-selected RFF when the same device is selected again.
- [ ] Fall back to the default RFF selection when no persisted RFF exists or the persisted RFF is no longer available.
- [ ] Add Settings, Help, and About buttons in the lower right outside the startup form.
- [ ] Handle empty-device, device-removed, and no-compatible-format states.

## 5. Sheets

- [ ] Implement the Settings sheet.
- [ ] Add setting for whether video stats are shown.
- [ ] Add setting for video stats location.
- [ ] Add setting for low-frame-rate warning visibility.
- [ ] Add image processing settings:
  - [ ] Horizontal flip.
  - [ ] Vertical flip if useful for parity with other ports.
  - [ ] Rotation options.
- [ ] Persist all settings and reload them across app launches and playback sessions.
- [ ] Implement the Help sheet with basic usage guidance.
- [ ] Implement the About sheet with:
  - [ ] Creator information.
  - [ ] License summary.
  - [ ] Privacy summary.
  - [ ] Build info.
  - [ ] Link to `https://github.com/centennial-oss/consolation-linux`.
  - [ ] Link to `https://centennialoss.org/privacy/`.

## 6. Complete Playback UX With Mock Backend

- [ ] Hide the startup form when playback starts.
- [ ] Show mock video playback in the main window.
- [ ] Preserve video aspect ratio at all times.
- [ ] Fit playback as large as possible within the window without clipping or skewing.
- [ ] Add the playback controls bar.
- [ ] Add Power button to stop playback and return to stopped state.
- [ ] Add app volume slider independent of OS volume.
- [ ] Persist volume preference across launches and playback sessions.
- [ ] Add zoom slider.
- [ ] Do not persist zoom after playback ends.
- [ ] Add settings cog button that opens the Settings sheet during playback.
- [ ] Auto-hide playback controls after 3 seconds of mouse idle.
- [ ] Show playback controls whenever the mouse moves over the main window during playback.
- [ ] Implement click-and-drag panning when zoomed.
- [ ] Clamp pan offsets so the user cannot drag beyond the video bounds.
- [ ] Add video stats overlay using mock frame timing.
- [ ] Add low-frame-rate warning behavior using mock frame timing.
- [ ] Verify the UX against the reference Windows, macOS/iOS, and Android screenshots.

## 7. Rendering Architecture

- [ ] Decide the first renderer implementation:
  - [ ] Qt RHI/OpenGL/Vulkan path for GPU import and shader conversion.
  - [ ] Fallback CPU upload path for systems without working DMABUF import.
- [ ] Define render input contracts for:
  - [ ] DMABUF-backed YUYV/NV12/RGB/BGR frames.
  - [ ] CPU-decoded MJPEG frames.
- [ ] Implement shader-based YUV to RGB conversion for YUYV/YUY2 and NV12.
- [ ] Implement RGB/BGR rendering.
- [ ] Implement CPU-decoded MJPEG upload path.
- [ ] Confirm zoom, pan, rotation, and flip are applied in the renderer instead of mutating frame buffers.
- [ ] Avoid per-frame allocations in the render path.

## 8. Makefile And Gitignore

- [ ] Add a Linux-appropriate `Makefile` after the app has a buildable binary.
- [ ] Include practical Makefile targets:
  - [ ] `make build`
  - [ ] `make run`
  - [ ] `make test`
  - [ ] `make clean`
  - [ ] `make build-release`
  - [ ] `make install-deps` or a documented distro-specific alternative if install automation is not desirable.
- [ ] Add a Linux/C++/Qt/CMake-appropriate `.gitignore`.
- [ ] Ignore local build directories, generated binaries, editor state, CMake output, Qt generated files, coverage output, logs, and package artifacts.
- [ ] Keep source assets, project files, docs, and reference material tracked.

## 9. Real V4L2 Video Device Discovery

- [ ] Enumerate `/dev/videoN` devices.
- [ ] Filter to UVC/capture-capable devices.
- [ ] Read stable device identity from v4l2 capabilities and sysfs.
- [ ] Collect USB bus/device path, vendor/product ID, serial, and physical parent where available.
- [ ] Implement hotplug detection with udev or a comparable Linux-native mechanism.
- [ ] Map V4L2 formats into the shared RFF model.
- [ ] Preserve the same UX behavior used by the mock backend.
- [ ] Add defensive handling for devices that report invalid, duplicated, or incomplete modes.

## 10. Real V4L2 Video Playback

- [ ] Open the selected `/dev/videoN` device with the selected RFF.
- [ ] Configure streaming buffers.
- [ ] Implement DMABUF buffer handling for zero-copy-capable formats.
- [ ] Implement the V4L2 dequeue/enqueue loop without per-frame mallocs.
- [ ] Add frame pool or ring-buffer ownership rules.
- [ ] Deliver YUYV/YUY2, NV12, RGB, and BGR frames to the renderer through DMABUF where possible.
- [ ] Detect unsupported DMABUF paths and fall back gracefully.
- [ ] Integrate libjpeg-turbo for MJPEG decompression.
- [ ] Reuse MJPEG decode buffers where possible.
- [ ] Keep MJPEG pipeline explicit as `V4L2 buffer -> libjpeg-turbo decode -> GPU upload`.
- [ ] Add stop, restart, and device removal handling.
- [ ] Add error surfaces that are understandable without exposing raw kernel details as the main message.

## 11. Audio Capture And Playback

- [ ] Implement capture-device to audio-device pairing using:
  - [ ] USB bus/device path.
  - [ ] Vendor/product ID.
  - [ ] Serial number.
  - [ ] Physical parent in sysfs.
- [ ] Implement PipeWire audio capture and playback as the primary path.
- [ ] Implement ALSA fallback.
- [ ] Default to lowest-latency mode:
  - [ ] Small PipeWire quantum/buffer size where possible.
  - [ ] No resampling unless required.
  - [ ] No audio processing.
- [ ] Support expected 48 kHz stereo PCM.
- [ ] Handle 44.1 kHz, mono, and unusual channel maps.
- [ ] Add a small audio jitter buffer.
- [ ] Sync audio against video presentation time without assuming audio and video timestamps share the same clock.
- [ ] Apply the app volume setting independently of OS volume.
- [ ] Keep audio start/stop coordinated with video playback lifecycle.

## 12. Performance And Reliability

- [ ] Profile startup, device enumeration, playback startup latency, steady-state CPU usage, and frame latency.
- [ ] Remove avoidable allocations from hot loops.
- [ ] Add buffer pools or ring buffers where measurements show churn.
- [ ] Add logging that is useful for diagnosing capture devices without recording user media.
- [ ] Ensure the app makes no outbound network requests and listens on no inbound network sockets.
- [ ] Test unplug/replug during stopped state.
- [ ] Test unplug/replug during active playback.
- [ ] Test devices that only expose MJPEG.
- [ ] Test devices that expose NV12 or YUYV at 1080p60.
- [ ] Test high-DPI displays, window resizing, and fullscreen behavior if supported.
- [ ] Test Wayland and X11.
- [ ] Test on Intel/AMD desktop Linux and Raspberry Pi OS.

## 13. Packaging And Distribution

- [ ] Add Linux desktop metadata:
  - [ ] `.desktop` file.
  - [ ] AppStream metadata.
  - [ ] Icon installation layout.
- [ ] Decide first package targets:
  - [ ] AppImage, Flatpak, distro packages, or source-only initial release.
- [ ] Document runtime dependencies.
- [ ] Document developer dependencies.
- [ ] Add release build instructions.
- [ ] Add troubleshooting notes for capture-card permissions, PipeWire, ALSA fallback, and Wayland/X11 renderer issues.

## 14. Final Validation

- [ ] Build from a clean checkout using only documented dependencies.
- [ ] Run tests.
- [ ] Launch the app with mock backend.
- [ ] Launch the app with at least one real UVC capture card.
- [ ] Verify stopped-state UX.
- [ ] Verify Settings, Help, and About sheets.
- [ ] Verify playback controls, auto-hide behavior, volume, zoom, and pan.
- [ ] Verify persisted settings and per-device RFF recall.
- [ ] Verify video stats and low-frame-rate warning.
- [ ] Verify privacy expectations: no recording, no saving, no streaming, no analytics, no network activity.
- [ ] Update `README.md` screenshots and supported-device notes.
