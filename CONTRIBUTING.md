# Contributing to Virtual Camera Engine

Thank you for helping! Bug reports, documentation fixes, tests on new distributions and code are all
welcome. This guide explains how to set up, what the code style is, and how to send a change.

## Ways to help

- **Report a bug** — open an issue with the *Bug report* form. Please include the output of
  `./build/bin/virtual-camera --version`, your distribution, and the full terminal output with `--verbose`.
- **Test on your system** — run `make test` (and `make device-test` if you have v4l2loopback) and tell us
  the result for your distribution and CPU.
- **Improve documentation** — anything that confused you is worth fixing.
- **Write code** — check the open issues, or open one first to discuss larger changes.

## Development setup

```bash
git clone <your-fork-url> virtual-camera-engine
cd virtual-camera-engine
make install-deps        # compilers, CMake, FFmpeg, GoogleTest, v4l2loopback
make                     # build into ./build
make test                # 173 tests, no camera needed
```

Useful while working on one part (details in [docs/BUILD.md](docs/BUILD.md)):

```bash
make module-buffer                                   # build one module
make test-buffer                                     # its tests only
ctest --test-dir build -R StreamBuffer --output-on-failure
gdb --args ./build/bin/test_buffer --gtest_filter='StreamBuffer*'
```

## Code style

The project favours **efficient algorithms written in easy-to-read code**:

- Prefer clear names and a few more lines over clever one-liners.
- **Explain** non-obvious system or library calls where they are used, e.g.
  `// clock_nanosleep(TIMER_ABSTIME) sleeps until an absolute time, so errors never add up`.
- Every new class or function gets a short comment saying *what it is for*, not only what it does.
- C++20, no exceptions across module boundaries: functions return `Status` / `Result<T>`.
- No memory allocation, locks (except the documented mailbox) or I/O in the pacer thread's per-frame path.
- Keep the compiler warning-free: CI builds with `-DVCAM_WARNINGS_AS_ERRORS=ON`.
- Shell scripts: `bash`, `set -euo pipefail`, quote variables.

### Module rules

Each folder in `modules/` is a separate library. A module may only include headers of modules it links
(see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) §1.2) — the build enforces this. If a change needs a new
dependency between modules, explain why in the pull request; architecture changes are documented in
`docs/ARCHITECTURE.md` §16, never made silently.

### Tests

- New behaviour needs a test in the module's `tests/` folder (GoogleTest).
- Prefer deterministic tests: use `SimulatedClock` / fast mode and the frame-number barcode of the test
  pattern instead of sleeping and guessing.
- Before sending a change run at least:

```bash
make test
make test-debug          # AddressSanitizer + UBSan
make test-tsan           # ThreadSanitizer, if you touched threads
```

## Sending a change

1. Fork the repository and create a branch: `git checkout -b fix/seek-in-ts-files`.
2. Make focused commits with clear messages in the imperative mood:

   ```text
   Fix seek landing late in MPEG-TS files

   TS files have no keyframe index, so av_seek_frame can land after the
   target. Retry with an earlier start until a keyframe before the target
   is found.
   ```

3. Update the documentation (`README.md`, `docs/`) and `CHANGELOG.md` (section *Unreleased*) when
   behaviour or options change.
4. Push and open a pull request; fill in the checklist in the template.
5. CI must pass (build + tests on several distributions, sanitizers, packaging).

By contributing you agree that your contribution is licensed under the project's [MIT License](LICENSE).

## Releasing (maintainer)

1. Update the version in `CMakeLists.txt` (`project(... VERSION x.y.z)`) and `CHANGELOG.md`.
2. Commit, then tag: `git tag -a vX.Y.Z -m "Version X.Y.Z" && git push --tags`.
3. The CI attaches the built `.deb` and `.tar.gz` packages to the GitHub release created from the tag.

## Code of Conduct

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
