#!/usr/bin/env bash

set -euo pipefail

with_packaging=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-packaging)
      with_packaging=true
      shift
      ;;
    *)
      echo "Unknown argument: $1" >&2
      echo "Usage: $0 [--with-packaging]" >&2
      exit 1
      ;;
  esac
done

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  if ! command -v sudo >/dev/null 2>&1; then
    echo "This script must be run as root (e.g. sudo $0)" >&2
    exit 1
  fi
  exec sudo "$0" "$@"
fi

if ! command -v dnf >/dev/null 2>&1; then
  echo "dnf is required but was not found on PATH." >&2
  exit 1
fi

packages=(
  cmake
  gcc-c++
  libjpeg-turbo-devel
  libva-devel
  mesa-libEGL-devel
  mesa-libGL-devel
  ninja-build
  pipewire-devel
  qt6-qtbase-devel
  qt6-qtsvg-devel
)

if [[ "$with_packaging" == "true" ]]; then
  packages+=(rpm-build)
fi

dnf install -y "${packages[@]}"
