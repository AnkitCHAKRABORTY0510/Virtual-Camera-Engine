# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[Semantic Versioning](https://semver.org/).

## [1.1.0] — 2026-10-01

### Added
- **Stream buffer mode (`--buffer-mode stream`), now the default**: only ~1 s of decoded frames is kept
  ahead of the camera, so memory use no longer depends on the video's length
  (15-minute 800×410 clip: ~65 MB instead of ~16.5 GiB). Options `--read-ahead` and `--decoder-threads`.
- Frame-exact seeking for video files, image sequences and the test pattern.
- Decoder runs at lower CPU priority so the frame clock always wins the CPU under load.
- `tools/install_deps.sh` / `make install-deps` for Ubuntu, Debian, Fedora, Arch and openSUSE.
- Clear configure-time checks (Linux, compiler, 64-bit, kernel headers, FFmpeg ≥ 4.4).
- `make install` / `make uninstall`, `make package` (.tar.gz, .deb, .rpm), `Dockerfile`.
- GitHub Actions CI on several distributions and compilers, sanitizers and packaging.
- User Guide, Full Reference, contributing guide and GitHub templates.

### Changed
- Status line shows how full the read-ahead window is (`ahead N/M`).
- `ram` and `disk` buffer modes remain available for machines that cannot decode in real time.

## [1.0.0] — 2026-10-01

### Added
- Virtual camera output through v4l2loopback, null and raw-file outputs.
- Inputs: video files (FFmpeg), image folders, test pattern with frame barcodes, script input over a Unix socket.
- Absolute-deadline scheduler, exact frame-rate conversion, pause / resume / seek, loop / hold / stop at end.
- RAM and disk buffers with memory checks, state machine, YAML config files, command line.
- Timing metrics and reports (text, JSON, CSV), debugging tools, 149 automated tests, documentation.

[1.1.0]: ../../compare/v1.0.0...v1.1.0
[1.0.0]: ../../releases/tag/v1.0.0
