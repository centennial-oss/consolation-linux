LINUX_HOST ?= user@linux-box
LINUX_DIR ?= ~/src/consolation-linux

sync-linux:
	rsync -az --delete \
		--exclude build \
		--exclude localdata \
		--exclude reference \
		--exclude .cache \
		./ $(LINUX_HOST):$(LINUX_DIR)/

build-linux: sync-linux
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && cmake -S . -B build && cmake --build build'

test-linux: build-linux
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && ctest --test-dir build --output-on-failure'

run-headless: build-linux
	ssh $(LINUX_HOST) 'cd $(LINUX_DIR) && ./build/headless_capture'
