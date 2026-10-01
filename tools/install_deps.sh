#!/usr/bin/env bash
# =============================================================================
# install_deps.sh — install everything Virtual Camera Engine needs, on any
# common Linux distribution.
#
#   bash tools/install_deps.sh              # build tools + FFmpeg + tests + camera module
#   bash tools/install_deps.sh --dry-run    # only PRINT the command it would run
#   bash tools/install_deps.sh --no-tests   # skip GoogleTest / ffmpeg CLI / OpenCV
#   bash tools/install_deps.sh --no-camera  # skip v4l2loopback (e.g. in a container or CI)
#
# Supported package managers (detected automatically):
#   apt     Ubuntu, Debian, Linux Mint, Pop!_OS, elementary, Kali, Raspberry Pi OS (64-bit)
#   dnf     Fedora, RHEL / Rocky / Alma (with EPEL + RPM Fusion)
#   pacman  Arch, Manjaro, EndeavourOS
#   zypper  openSUSE Tumbleweed / Leap
#
# How it works: one function per package manager returns the package list;
# the script runs a single install command with sudo (or directly as root).
# Nothing else on the system is changed.
# =============================================================================
set -euo pipefail

DRY_RUN=0
WITH_TESTS=1
WITH_CAMERA=1

for argument in "$@"; do
    case "$argument" in
        --dry-run)   DRY_RUN=1 ;;
        --no-tests)  WITH_TESTS=0 ;;
        --no-camera) WITH_CAMERA=0 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "unknown option: $argument (try --help)" >&2
            exit 1 ;;
    esac
done

# ---- helpers ----------------------------------------------------------------

have() { command -v "$1" >/dev/null 2>&1; }

# Runs a command as root: directly when we are root, otherwise through sudo.
as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif have sudo; then
        sudo "$@"
    else
        echo "ERROR: this needs root rights and 'sudo' is not installed. Run as root." >&2
        exit 1
    fi
}

run() {
    echo "+ $*"
    if [ "$DRY_RUN" -eq 0 ]; then
        as_root "$@"
    fi
}

# The running kernel's version, used for the kernel-headers package that
# v4l2loopback-dkms needs to compile the camera module.
KERNEL="$(uname -r)"

# ---- package lists per package manager --------------------------------------

apt_packages() {
    local list="build-essential cmake pkg-config git file libavformat-dev libavcodec-dev libavutil-dev libswscale-dev"
    if [ "$WITH_TESTS" -eq 1 ]; then
        list="$list libgtest-dev ffmpeg python3 python3-opencv"
    fi
    if [ "$WITH_CAMERA" -eq 1 ]; then
        list="$list v4l-utils v4l2loopback-dkms v4l2loopback-utils"
        # Kernel headers: exact package for the running kernel if it exists.
        if apt-cache show "linux-headers-$KERNEL" >/dev/null 2>&1; then
            list="$list linux-headers-$KERNEL"
        fi
    fi
    echo "$list"
}

dnf_packages() {
    # 'pkgconfig(name)' lets dnf pick whichever package provides that library
    # (ffmpeg-free on plain Fedora, ffmpeg from RPM Fusion when enabled).
    local list="gcc-c++ make cmake pkgconf-pkg-config git"
    list="$list pkgconfig(libavformat) pkgconfig(libavcodec) pkgconfig(libavutil) pkgconfig(libswscale)"
    if [ "$WITH_TESTS" -eq 1 ]; then
        list="$list gtest-devel /usr/bin/ffmpeg python3"
    fi
    if [ "$WITH_CAMERA" -eq 1 ]; then
        list="$list v4l-utils"   # v4l2loopback itself comes from RPM Fusion, see the note below
    fi
    echo "$list"
}

pacman_packages() {
    local list="base-devel cmake pkgconf git ffmpeg"
    if [ "$WITH_TESTS" -eq 1 ]; then
        list="$list gtest python python-opencv"
    fi
    if [ "$WITH_CAMERA" -eq 1 ]; then
        list="$list v4l-utils v4l2loopback-dkms linux-headers"
    fi
    echo "$list"
}

zypper_packages() {
    local list="gcc-c++ make cmake pkg-config git"
    list="$list pkgconfig(libavformat) pkgconfig(libavcodec) pkgconfig(libavutil) pkgconfig(libswscale)"
    if [ "$WITH_TESTS" -eq 1 ]; then
        list="$list googletest-devel /usr/bin/ffmpeg python3"
    fi
    if [ "$WITH_CAMERA" -eq 1 ]; then
        list="$list v4l-utils v4l2loopback-kmp-default v4l2loopback-utils"
    fi
    echo "$list"
}

# ---- detect and install -----------------------------------------------------

DISTRO="unknown"
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091  # the file exists on every systemd-era distribution
    . /etc/os-release
    DISTRO="${PRETTY_NAME:-${ID:-unknown}}"
fi
echo "Detected system: $DISTRO ($(uname -m))"

if [ "$(uname -s)" != "Linux" ]; then
    echo "ERROR: Virtual Camera Engine needs Linux (it uses the Linux v4l2loopback camera driver)." >&2
    exit 1
fi

if have apt-get; then
    run apt-get update
    # shellcheck disable=SC2046  # word splitting of the package list is intended
    run env DEBIAN_FRONTEND=noninteractive apt-get install -y $(apt_packages)
elif have dnf; then
    # shellcheck disable=SC2046
    run dnf install -y $(dnf_packages)
    if [ "$WITH_CAMERA" -eq 1 ]; then
        cat <<'END_OF_NOTE'

NOTE (Fedora/RHEL): the v4l2loopback camera module is in RPM Fusion. Enable it once, then install:
  sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
  sudo dnf install v4l2loopback
For full codec support (H.264/H.265) replace ffmpeg-free with RPM Fusion's ffmpeg:
  sudo dnf swap ffmpeg-free ffmpeg --allowerasing
END_OF_NOTE
    fi
elif have pacman; then
    # shellcheck disable=SC2046
    run pacman -Syu --needed --noconfirm $(pacman_packages)
elif have zypper; then
    # shellcheck disable=SC2046
    run zypper --non-interactive install $(zypper_packages)
    echo
    echo "NOTE (openSUSE): for H.264/H.265 decoding use FFmpeg from the Packman repository."
else
    cat >&2 <<'END_OF_NOTE'
ERROR: no supported package manager found (apt, dnf, pacman, zypper).
Install these yourself, then run 'make':
  * a C++20 compiler (GCC >= 10 or Clang >= 12), CMake >= 3.20, pkg-config, make
  * FFmpeg development files: libavformat, libavcodec, libavutil, libswscale (FFmpeg >= 4.4)
  * the v4l2loopback kernel module (+ v4l-utils)
  * optional, for tests: GoogleTest, the ffmpeg command, python3
END_OF_NOTE
    exit 1
fi

echo
if [ "$DRY_RUN" -eq 1 ]; then
    echo "Dry run: nothing was installed."
else
    echo "Done. Next steps:"
    echo "  make                    # build"
    echo "  make setup-loopback     # create the camera /dev/video10"
    echo "  make run FILE=video.mp4 # play a video as the camera"
fi
