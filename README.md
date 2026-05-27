# <img src="assets/icons/app-icon.png" alt="Consolation" height="48" /> Consolation™


A 100% free, no-frills, incredibly performant video capture viewer for Linux with no analytics or snooping.

Installers are available for the following platforms in the Releases:
 * Fedora 44 (x86_64 and aarch64)
 * Fedora 42 (x86_64 and aarch64)
 * Ubuntu 24.04 (amd64 and arm64)
 * Ubuntu 26.04 (amd64 and arm64)
 * Raspberry Pi OS Trixie

## About

Consolation is a free app that enables your Linux workstation or RPi to be used as a display for devices like gaming consoles, other Raspberry Pis, and even a Mac mini, Linux or Windows PC, via a standard USB Video Class (UVC) video capture card.

The app is intentionally simple: watch the live video on your computer. No recording or saving, no streaming to the internet. Just plug and play, privately with no ads or tracking. Consolation will never make an outbound network request or listen for inbound network connections.

## Screenshots

Coming soon.


## Privacy

Consolation does not collect, send, or share your data. Audio and video stay local and transient while you are watching a connected capture device. The app is open source, contains no trackers or analytics, makes no network calls, and does not record, stream, save, or analyze audio or video. Consolation has no idea what content is coming through your capture card's feed, and nothing leaves your device, ever.

Read the full privacy policy at [PRIVACY.md](PRIVACY.md) or <https://centennialoss.org/privacy/>.

## Supported Capture Devices

Any capture device that appears to Linux as a USB Video Class (UVC) capture device should work with Consolation.

Linux capture device testing is in progress. The other Consolation ports have been tested with these UVC capture devices:

- Elgato HD60 X - 👌 🚀
- Acer USB 3.0 Video Capture Card (model OCB5B0) - 👌 🚀
- WANKEDA 4K Capture Card 1080p 60FPS for Streaming (1da603d4) - 👌 🚀
- blueAVS 4K Capture Card (A3-B) - 👌 🚀
- Guermok Video Capture Card (GM-29A) - 👌 🚀
- PERESAL USB 3.0 Video Capture Card with PD 100W - 👌 🚀
- UGREEN Full HD 1080p Capture Card (model 40189) -  ⚠️ max 30p @ 1920x1080

## TODO

- Automate release builds via GitHub Workflow
- Support Full Screen Mode (currently supports Windowed/Maximized)
- Support Stay-on-Top
- Support GUI Menu Icons to launch the App (currently must launch from terminal or Run dialog)

## Requirements

### Running

- A modern Linux distribution, recent as of January 2025, with support for Qt 6 and V4L2
- A UVC-compliant video capture card

`deb` and `rpm` installers place the binary in`/usr/bin/consolation`. It is recommended to run as an unprivileged user.

### Developer

- CMake 3.22 or newer
- C++20 compiler
- Qt 6.4 or newer development packages

See [DEVELOPER.md](DEVELOPER.md) for distro-specific setup commands.

#### Building

For a local dev build, run:

```sh
make build-linux
```

See the Makefile for release build targets.

## Contributor Disclosure

Humans write this software with AI assistance. All contributions are well-tested and merged only after being reviewed and approved by humans who fully understand and take responsibility for the contribution.

While we welcome pull requests and other contributions from other humans, including AI-generated code, we do not accept contributions from AI bots. A human must review, understand, and sign off on all commits. All contributors must be able to defend their contributions under reasonable technical scrutiny. Please file an issue to discuss any proposed feature before working on it.

## Trademark Notice

Consolation and its logo are trademarks of Centennial OSS Inc.
Use of the name and branding is not permitted for modified versions or forks without permission.
See [TRADEMARKS.md](TRADEMARKS.md) for details.
