LINUX_HOST ?= user@linux-box
LINUX_DIR ?= ~/src/consolation-linux
LINUX_BUILD_DIR ?= build-default
CMAKE_GENERATOR ?=
CMAKE_CONFIGURE_FLAGS ?=
CMAKE_GENERATOR_FLAG = $(if $(CMAKE_GENERATOR),-G "$(CMAKE_GENERATOR)",)
RELEASE_VERSION ?= localdev
BUILD_INFO_FILE ?= src/app/BuildInfo.h
BUILD_DATE := $(shell date -u +"%Y-%m-%dT%H:%M:%SZ")
GIT_COMMIT ?= $(shell if command -v git >/dev/null 2>&1; then git rev-parse HEAD; else echo localdev; fi)
BUILD_PLATFORM ?= Linux
BUILD_ARCHITECTURE ?= localdev
SET_BUILD_INFO_SCRIPT := scripts/set-build-info.sh

.PHONY: \
	build \
	test \
	sync-remote \
	build-remote \
	test-remote \
	test-linux \
	set-release-version-info \
	clear-version-info \
	build-fedora-42-binary \
	build-fedora-43-binary \
	build-fedora-44-binary \
	package-fedora-42-amd64 \
	package-fedora-42-arm64 \
	package-fedora-43-amd64 \
	package-fedora-43-arm64 \
	package-fedora-44-amd64 \
	package-fedora-44-arm64 \
	build-ubuntu-22-binary \
	build-ubuntu-24-binary \
	build-ubuntu-26-binary \
	package-ubuntu-22-amd64 \
	package-ubuntu-22-arm64 \
	package-ubuntu-24-amd64 \
	package-ubuntu-24-arm64 \
	package-ubuntu-26-amd64 \
	package-ubuntu-26-arm64 \
	build-rpi-os-trixie-binary \
	test-linux-rpi-os-trixie \
	package-rpi-os-trixie-arm64


# Local build (on linux workstation)

build:
	cd $(LINUX_DIR) && cmake -S . -B $(LINUX_BUILD_DIR) $(CMAKE_GENERATOR_FLAG) $(CMAKE_CONFIGURE_FLAGS) && cmake --build $(LINUX_BUILD_DIR)

test: build
	cd $(LINUX_DIR) && ctest --test-dir $(LINUX_BUILD_DIR) --output-on-failure


# Remote build (from non-linux workstation to linux workstation)

sync-remote:
	rsync -az --delete \
		--exclude 'build*' \
		--exclude localdata \
		--exclude reference \
		--exclude .cache \
		./ $(LINUX_HOST):$(LINUX_DIR)/

build-remote: sync-remote
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && make build'

test-remote: sync-remote
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && make build test'

# release build metadata injection

set-release-version-info:
	bash $(SET_BUILD_INFO_SCRIPT) --path "$(BUILD_INFO_FILE)" --version "$(RELEASE_VERSION)" --build-date "$(BUILD_DATE)" --build-platform "$(BUILD_PLATFORM)" --build-architecture "$(BUILD_ARCHITECTURE)" --git-commit "$(GIT_COMMIT)"

clear-version-info:
	bash $(SET_BUILD_INFO_SCRIPT) --path "$(BUILD_INFO_FILE)"

# os-version-specific binary and package builds

## fedora

### binaries

build-fedora-42-binary:
	cmake --preset fedora-42
	cmake --build --preset fedora-42

build-fedora-43-binary:
	cmake --preset fedora-43
	cmake --build --preset fedora-43

build-fedora-44-binary:
	cmake --preset fedora-44
	cmake --build --preset fedora-44

### packages

package-fedora-42-amd64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=amd64; \
	$(MAKE) build-fedora-42-binary; \
	cd build-fedora-42 && cpack -G RPM -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_RELEASE=1 -D CPACK_RPM_PACKAGE_ARCHITECTURE=x86_64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-fedora-42-x86_64

package-fedora-42-arm64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=aarch64; \
	$(MAKE) build-fedora-42-binary; \
	cd build-fedora-42 && cpack -G RPM -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_RELEASE=1 -D CPACK_RPM_PACKAGE_ARCHITECTURE=aarch64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-fedora-42-aarch64

package-fedora-43-amd64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=amd64; \
	$(MAKE) build-fedora-43-binary; \
	cd build-fedora-43 && cpack -G RPM -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_RELEASE=1 -D CPACK_RPM_PACKAGE_ARCHITECTURE=x86_64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-fedora-43-x86_64

package-fedora-43-arm64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=aarch64; \
	$(MAKE) build-fedora-43-binary; \
	cd build-fedora-43 && cpack -G RPM -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_RELEASE=1 -D CPACK_RPM_PACKAGE_ARCHITECTURE=aarch64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-fedora-43-aarch64

package-fedora-44-amd64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=amd64; \
	$(MAKE) build-fedora-44-binary; \
	cd build-fedora-44 && cpack -G RPM -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_RELEASE=1 -D CPACK_RPM_PACKAGE_ARCHITECTURE=x86_64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-fedora-44-x86_64

package-fedora-44-arm64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=aarch64; \
	$(MAKE) build-fedora-44-binary; \
	cd build-fedora-44 && cpack -G RPM -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_RPM_PACKAGE_RELEASE=1 -D CPACK_RPM_PACKAGE_ARCHITECTURE=aarch64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-fedora-44-aarch64

## ubuntu

### binaries

build-ubuntu-22-binary:
	cmake --preset ubuntu-22
	cmake --build --preset ubuntu-22

build-ubuntu-24-binary:
	cmake --preset ubuntu-24
	cmake --build --preset ubuntu-24

build-ubuntu-26-binary:
	cmake --preset ubuntu-26
	cmake --build --preset ubuntu-26

### packages

package-ubuntu-22-amd64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=amd64; \
	$(MAKE) build-ubuntu-22-binary; \
	cd build-ubuntu-22 && cpack -G DEB -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=amd64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-ubuntu-jammy-amd64

package-ubuntu-22-arm64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=arm64; \
	$(MAKE) build-ubuntu-22-binary; \
	cd build-ubuntu-22 && cpack -G DEB -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=arm64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-ubuntu-jammy-arm64

package-ubuntu-24-amd64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=amd64; \
	$(MAKE) build-ubuntu-24-binary; \
	cd build-ubuntu-24 && cpack -G DEB -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=amd64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-ubuntu-noble-amd64

package-ubuntu-24-arm64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=arm64; \
	$(MAKE) build-ubuntu-24-binary; \
	cd build-ubuntu-24 && cpack -G DEB -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=arm64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-ubuntu-noble-arm64

package-ubuntu-26-amd64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=amd64; \
	$(MAKE) build-ubuntu-26-binary; \
	cd build-ubuntu-26 && cpack -G DEB -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=amd64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-ubuntu-resolute-amd64

package-ubuntu-26-arm64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=arm64; \
	$(MAKE) build-ubuntu-26-binary; \
	cd build-ubuntu-26 && cpack -G DEB -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=arm64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-ubuntu-resolute-arm64

## Raspberry Pi OS Builds

### binaries

build-rpi-os-trixie-binary:
	cmake --preset rpi-os-trixie
	cmake --build --preset rpi-os-trixie

test-linux-rpi-os-trixie: build-rpi-os-trixie-binary
	ctest --preset rpi-os-trixie

### packages

package-rpi-os-trixie-arm64:
	@trap '$(MAKE) -C "$(CURDIR)" clear-version-info' EXIT; \
	$(MAKE) set-release-version-info BUILD_ARCHITECTURE=arm64; \
	$(MAKE) build-rpi-os-trixie-binary; \
	cd build-rpi-os-trixie && cpack -G DEB -D CPACK_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_VERSION=$(RELEASE_VERSION) -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=arm64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(RELEASE_VERSION)-rpi-os-trixie-arm64
