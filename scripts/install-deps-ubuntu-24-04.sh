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
  libpipewire-0.3-dev
  ninja-build
  qt6-base-dev
  qt6-base-dev-tools
  libqt6svg6-dev
)

if [[ "$with_packaging" == "true" ]]; then
  packages+=(dpkg-dev)
fi

sudo apt install -y "${packages[@]}"
