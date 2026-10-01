#!/usr/bin/env bash
# =============================================================================
# setup_loopback.sh — create (or remove) the v4l2loopback virtual camera device
#
# Needs root because it loads a kernel module. Run once per boot:
#
#   sudo tools/setup_loopback.sh                         # /dev/video10, "Virtual Camera Engine"
#   sudo tools/setup_loopback.sh --number 11 --name "My Cam"
#   sudo tools/setup_loopback.sh --reload                # unload first (changes name/options)
#   sudo tools/setup_loopback.sh --remove                # unload the module
#
# Options used (docs/ARCHITECTURE.md §3.2):
#   video_nr=N         stable device path /dev/videoN
#   card_label="..."   the camera name applications display
#   exclusive_caps=1   REQUIRED for Chrome/WebRTC: device shows as a capture-only
#                      camera while the engine is streaming into it
#   max_buffers=2      low latency
#
# To load it automatically at every boot, see docs/README section "Persistent setup".
# =============================================================================
set -euo pipefail

NUMBER=10
NAME="Virtual Camera Engine"
RELOAD=0
REMOVE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --number) NUMBER="$2"; shift 2 ;;
        --name)   NAME="$2"; shift 2 ;;
        --reload) RELOAD=1; shift ;;
        --remove) REMOVE=1; shift ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "This script loads a kernel module and must run as root: sudo $0 $*" >&2
    exit 1
fi

loaded() { lsmod | grep -q '^v4l2loopback'; }

if [ "$REMOVE" -eq 1 ]; then
    if loaded; then
        modprobe -r v4l2loopback && echo "v4l2loopback unloaded"
    else
        echo "v4l2loopback is not loaded"
    fi
    exit 0
fi

if ! modinfo v4l2loopback >/dev/null 2>&1; then
    cat >&2 <<EOF
The v4l2loopback kernel module is not installed. Install it with:
    bash tools/install_deps.sh        (detects apt / dnf / pacman / zypper)
or by hand:
    Ubuntu/Debian : sudo apt install v4l2loopback-dkms v4l2loopback-utils linux-headers-\$(uname -r)
    Fedora        : sudo dnf install v4l2loopback          (RPM Fusion repository)
    Arch          : sudo pacman -S v4l2loopback-dkms linux-headers
    openSUSE      : sudo zypper install v4l2loopback-kmp-default
If Secure Boot is enabled (mokutil --sb-state), the DKMS build asks you to
enrol a key (MOK) on the next reboot; the module loads only after that.
EOF
    exit 1
fi

if loaded && [ "$RELOAD" -eq 1 ]; then
    echo "unloading v4l2loopback (close every program using a loopback camera first)..."
    modprobe -r v4l2loopback
fi

if loaded; then
    # Module already present: add a device at runtime if this version supports it.
    if [ -e "/dev/video$NUMBER" ]; then
        echo "/dev/video$NUMBER already exists (use --reload to recreate it with new options)"
    elif command -v v4l2loopback-ctl >/dev/null 2>&1 && v4l2loopback-ctl add --help >/dev/null 2>&1; then
        v4l2loopback-ctl add -n "$NAME" -x 1 -b 2 "/dev/video$NUMBER"
    else
        echo "v4l2loopback is loaded with other devices and cannot add one at runtime; use --reload" >&2
        exit 1
    fi
else
    modprobe v4l2loopback devices=1 video_nr="$NUMBER" card_label="$NAME" exclusive_caps=1 max_buffers=2
fi

sleep 0.5
if [ ! -e "/dev/video$NUMBER" ]; then
    echo "device /dev/video$NUMBER did not appear; check: dmesg | tail" >&2
    exit 1
fi
echo "virtual camera device ready: /dev/video$NUMBER"
if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl -d "/dev/video$NUMBER" --info | sed 's/^/  /'
fi
echo "Next: ./build/bin/virtual-camera --input video.mp4 --device /dev/video$NUMBER"
