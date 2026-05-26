LINUX_HOST ?= user@linux-box
LINUX_DIR ?= ~/src/consolation-linux
LINUX_BUILD_DIR ?= build-linux-default
CMAKE_GENERATOR ?=
CMAKE_CONFIGURE_FLAGS ?=
CMAKE_GENERATOR_FLAG = $(if $(CMAKE_GENERATOR),-G "$(CMAKE_GENERATOR)",)
PACKAGE_VERSION ?= 0.1.0

.PHONY: \
	sync-linux \
	build-linux \
	test-linux \
	run-headless \
	build-fedora-44-binary \
	build-fedora-44 \
	build-fedora-44-amd64 \
	build-fedora-44-arm64 \
	build-ubuntu-24-04-binary \
	build-ubuntu-24-04 \
	build-ubuntu-24-04-amd64 \
	build-ubuntu-24-04-arm64 \
	build-linux-ubuntu-2404 \
	test-linux-ubuntu-2404 \
	build-linux-fedora-44 \
	test-linux-fedora-44 \
	build-linux-rpi-os-trixie \
	test-linux-rpi-os-trixie

build-fedora-44-binary:
	cmake --preset fedora-44
	cmake --build --preset fedora-44

build-ubuntu-24-04-binary:
	cmake --preset ubuntu-2404
	cmake --build --preset ubuntu-2404

sync-linux:
	rsync -az --delete \
		--exclude 'build*' \
		--exclude localdata \
		--exclude reference \
		--exclude .cache \
		./ $(LINUX_HOST):$(LINUX_DIR)/

build-linux: sync-linux
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && cmake -S . -B $(LINUX_BUILD_DIR) $(CMAKE_GENERATOR_FLAG) $(CMAKE_CONFIGURE_FLAGS) && cmake --build $(LINUX_BUILD_DIR)'

test-linux: build-linux
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && ctest --test-dir $(LINUX_BUILD_DIR) --output-on-failure'

run-headless: build-linux
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && ./$(LINUX_BUILD_DIR)/headless_capture $(HEADLESS_ARGS)'

build-fedora-44: build-fedora-44-amd64 build-fedora-44-arm64

build-fedora-44-amd64: build-fedora-44-binary
	cd build-fedora-44 && cpack -G RPM -D CPACK_RPM_PACKAGE_ARCHITECTURE=x86_64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(PACKAGE_VERSION)-fedora-44-x86_64

build-fedora-44-arm64: build-fedora-44-binary
	cd build-fedora-44 && cpack -G RPM -D CPACK_RPM_PACKAGE_ARCHITECTURE=aarch64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(PACKAGE_VERSION)-fedora-44-aarch64

build-ubuntu-24-04: build-ubuntu-24-04-amd64 build-ubuntu-24-04-arm64

build-ubuntu-24-04-amd64: build-ubuntu-24-04-binary
	cd build-ubuntu-2404 && cpack -G DEB -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=amd64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(PACKAGE_VERSION)-ubuntu-24.04-amd64

build-ubuntu-24-04-arm64: build-ubuntu-24-04-binary
	cd build-ubuntu-2404 && cpack -G DEB -D CPACK_DEBIAN_PACKAGE_ARCHITECTURE=arm64 -D CPACK_PACKAGE_FILE_NAME=consolation-$(PACKAGE_VERSION)-ubuntu-24.04-arm64

build-linux-ubuntu-2404: build-ubuntu-24-04-binary

test-linux-ubuntu-2404: build-linux-ubuntu-2404
	ctest --preset ubuntu-2404

build-linux-fedora-44: build-fedora-44-binary

test-linux-fedora-44: build-linux-fedora-44
	ctest --preset fedora-44

build-linux-rpi-os-trixie:
	cmake --preset rpi-os-trixie
	cmake --build --preset rpi-os-trixie

test-linux-rpi-os-trixie: build-linux-rpi-os-trixie
	ctest --preset rpi-os-trixie
