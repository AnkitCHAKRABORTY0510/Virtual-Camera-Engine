# =============================================================================
# Makefile — convenience wrapper around CMake
#
# CMake is the real build system; this file only gives short, memorable
# commands. Run `make help` for the list.
#
# Variables you can override on the command line:
#   BUILD_DIR   build directory            (default: build)
#   BUILD_TYPE  Release | Debug | RelWithDebInfo (default: Release)
#   JOBS        parallel compile jobs      (default: number of CPU cores)
#   CMAKE_FLAGS extra -D options for CMake (e.g. CMAKE_FLAGS="-DVCAM_BUILD_TESTS=OFF")
#   FILE        input for run / probe-source (video file, image directory or "pattern")
#   DEVICE      v4l2loopback device         (default: /dev/video10)
#   ARGS        extra arguments for the program being run
# =============================================================================

BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
JOBS        ?= $(shell nproc 2>/dev/null || echo 4)
CMAKE_FLAGS ?=
DEVICE      ?= /dev/video10

DEBUG_DIR   := build-debug
TSAN_DIR    := build-tsan
MODULES     := core convert source buffer timing metrics resample output config control
BIN         := ./$(BUILD_DIR)/bin

.DEFAULT_GOAL := all

.PHONY: all help configure build release debug tsan test test-debug test-tsan test-integration \
        $(addprefix module-,$(MODULES)) $(addprefix test-,$(MODULES)) \
        run run-pattern run-push push-example probe-source bench resample-table probe-device \
        setup-loopback remove-loopback device-test media env-check deps install-deps install uninstall \
        package docker-build docker-run clean distclean

## ---- Full build ---------------------------------------------------------------

all: build ## Configure and build everything (Release)

configure: ## Run CMake configure step only
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(CMAKE_FLAGS)

build: configure ## Build all modules, the app, tools and tests
	cmake --build $(BUILD_DIR) -j $(JOBS)

release: ## Optimised build in ./build
	$(MAKE) build BUILD_TYPE=Release

debug: ## Debug build with AddressSanitizer + UBSan in ./build-debug
	$(MAKE) build BUILD_DIR=$(DEBUG_DIR) BUILD_TYPE=Debug \
		CMAKE_FLAGS="-DVCAM_ENABLE_ASAN=ON -DVCAM_ENABLE_UBSAN=ON $(CMAKE_FLAGS)"

tsan: ## Build with ThreadSanitizer (data-race detector) in ./build-tsan
	$(MAKE) build BUILD_DIR=$(TSAN_DIR) BUILD_TYPE=RelWithDebInfo CMAKE_FLAGS="-DVCAM_ENABLE_TSAN=ON $(CMAKE_FLAGS)"

install: build ## Install virtual-camera, vcam_probe, vcam-setup-loopback into /usr/local (asks for sudo)
	sudo cmake --install $(BUILD_DIR)

uninstall: ## Remove the files installed by 'make install' (asks for sudo)
	@if [ ! -f $(BUILD_DIR)/install_manifest.txt ]; then echo "nothing installed from $(BUILD_DIR)"; exit 0; fi
	@# install_manifest.txt lists every file 'cmake --install' copied.
	sudo xargs -d '\n' rm -fv < $(BUILD_DIR)/install_manifest.txt

package: build ## Build installable packages (.tar.gz, and .deb/.rpm when possible) into BUILD_DIR
	cd $(BUILD_DIR) && cpack
	@ls -1 $(BUILD_DIR)/virtual-camera-engine-* 2>/dev/null || true

## ---- Running the complete software -------------------------------------------

run: build ## Run the camera: make run FILE=video.mp4 ARGS="--fps 30 --on-eof loop"
	@if [ -z "$(FILE)" ]; then echo 'usage: make run FILE=path/to/video.mp4 [ARGS="--fps 30"]'; exit 1; fi
	$(BIN)/virtual-camera --input "$(FILE)" --device $(DEVICE) $(ARGS)

run-pattern: build ## Stream the built-in test pattern to DEVICE
	$(BIN)/virtual-camera --input pattern --device $(DEVICE) $(ARGS)

run-push: build ## Wait for a script to push frames (see push-example)
	$(BIN)/virtual-camera --source-type push --socket /tmp/vcam.sock --device $(DEVICE) $(ARGS)

push-example: ## Example Python producer for run-push
	python3 tools/examples/push_frames.py --socket /tmp/vcam.sock $(ARGS)

setup-loopback: ## Create /dev/video10 (asks for sudo)
	sudo bash tools/setup_loopback.sh $(ARGS)

remove-loopback: ## Unload v4l2loopback (asks for sudo)
	sudo bash tools/setup_loopback.sh --remove

## ---- Tests --------------------------------------------------------------------

test: build ## Build and run every test (unit + integration)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

test-debug: debug ## Run every test under ASan + UBSan
	ctest --test-dir $(DEBUG_DIR) --output-on-failure

test-tsan: tsan ## Run every test under ThreadSanitizer
	TSAN_OPTIONS="suppressions=$(CURDIR)/tests/tsan.supp halt_on_error=1" ctest --test-dir $(TSAN_DIR) --output-on-failure

test-integration: build ## Integration tests only (app CLI, ffprobe comparison, Python push)
	ctest --test-dir $(BUILD_DIR) -L integration --output-on-failure

device-test: build ## Tests on a real v4l2loopback device (run setup-loopback first)
	bash tests/e2e/run_device_tests.sh $(DEVICE)

## ---- Module by module ---------------------------------------------------------
# module-<name> builds only that module's library (and the modules it needs).
# test-<name>   builds and runs only that module's unit tests.

$(addprefix module-,$(MODULES)): module-%: configure
	cmake --build $(BUILD_DIR) -j $(JOBS) --target vcam_$*

$(addprefix test-,$(MODULES)): test-%: configure
	cmake --build $(BUILD_DIR) -j $(JOBS) --target test_$*
	ctest --test-dir $(BUILD_DIR) -L $* --output-on-failure

## ---- Debugging tools (one module each) -----------------------------------------

probe-source: configure ## Decode FILE with the source module only: make probe-source FILE=video.mp4
	@if [ -z "$(FILE)" ]; then echo 'usage: make probe-source FILE=path/to/video.mp4 [ARGS="--frames all"]'; exit 1; fi
	cmake --build $(BUILD_DIR) -j $(JOBS) --target probe_source
	$(BIN)/probe_source $(ARGS) "$(FILE)"

bench: configure ## Timing accuracy without a device: make bench ARGS="--fps 60 --seconds 30"
	cmake --build $(BUILD_DIR) -j $(JOBS) --target bench_scheduler
	$(BIN)/bench_scheduler $(ARGS)

resample-table: configure ## FPS mapping table: make resample-table ARGS="--source-fps 24 --output-fps 30"
	cmake --build $(BUILD_DIR) -j $(JOBS) --target resample_table
	$(BIN)/resample_table $(ARGS)

probe-device: configure ## Read the camera like an app: make probe-device ARGS="--frames 300 --barcode"
	cmake --build $(BUILD_DIR) -j $(JOBS) --target vcam_probe
	$(BIN)/vcam_probe --device $(DEVICE) $(ARGS)

media: ## Generate the test videos into BUILD_DIR/test_media
	bash tools/gen_test_media.sh $(BUILD_DIR)/test_media

env-check: ## Inspect this machine (read-only), writes env_report.txt
	bash tools/env_check.sh env_report.txt

## ---- Dependencies -------------------------------------------------------------

deps: ## Print the install command for this Linux distribution (installs nothing)
	@bash tools/install_deps.sh --dry-run

install-deps: ## Install all dependencies (apt, dnf, pacman or zypper; asks for sudo)
	bash tools/install_deps.sh

## ---- Docker (build and run without installing anything but Docker) -----------

DOCKER_IMAGE ?= virtual-camera-engine

docker-build: ## Build the Docker image (the camera module must still be loaded on the host)
	docker build -t $(DOCKER_IMAGE) .

docker-run: ## Run in Docker: make docker-run FILE=/abs/path/video.mp4 [ARGS=...]
	@if [ -z "$(FILE)" ]; then echo 'usage: make docker-run FILE=/absolute/path/to/video.mp4'; exit 1; fi
	docker run --rm -it --device $(DEVICE) -v "$(abspath $(dir $(FILE)))":/media:ro \
		$(DOCKER_IMAGE) --input "/media/$(notdir $(FILE))" --device $(DEVICE) $(ARGS)

## ---- Cleaning -----------------------------------------------------------------

clean: ## Remove compiled files (keeps CMake configuration)
	@if [ -d $(BUILD_DIR) ]; then cmake --build $(BUILD_DIR) --target clean; fi

distclean: ## Remove all build directories
	rm -rf build $(DEBUG_DIR) $(TSAN_DIR)

## ---- Help ---------------------------------------------------------------------

help: ## Show this help
	@echo "Virtual Camera Engine — make targets"
	@echo
	@grep -E '^[a-zA-Z_-]+:.*## ' $(MAKEFILE_LIST) | sed -E 's/:.*## /\t/' | awk -F'\t' '{ printf "  %-18s %s\n", $$1, $$2 }'
	@echo
	@echo "  module-<name>      Build one module library    (modules: $(MODULES))"
	@echo "  test-<name>        Build + run one module's tests, e.g. make test-timing"
	@echo
	@echo "Variables: BUILD_DIR=$(BUILD_DIR) BUILD_TYPE=$(BUILD_TYPE) JOBS=$(JOBS) DEVICE=$(DEVICE)"
