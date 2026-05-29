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

if ! command -v apt >/dev/null 2>&1; then
  echo "apt is required but was not found on PATH." >&2
  exit 1
fi

sudo apt update

packages=(
  build-essential
  cmake
  libegl-dev
  libgl-dev
  libjpeg62-turbo-dev
  libva-dev
  libpipewire-0.3-dev
  v4l-utils
  libxkbcommon-dev
  ninja-build
  pkg-config
  qt6-base-dev
  qt6-base-dev-tools
  qt6-svg-dev
)

if [[ "$with_packaging" == "true" ]]; then
  packages+=(dpkg-dev)
fi

sudo apt install -y "${packages[@]}"
