#!/usr/bin/env bash
# =============================================================================
# env_check.sh — Phase 0 environment inspection for the Virtual Camera Engine
#
# What it does:
#   Collects every fact the engine design depends on (OS, kernel, compilers,
#   FFmpeg libraries, V4L2 devices, v4l2loopback, Secure Boot, permissions,
#   real-time limits) and writes them to a report file.
#
# What it does NOT do:
#   It is READ-ONLY. It installs nothing, loads no kernel module and never
#   calls sudo. The commands that need root are only PRINTED at the end so you
#   can review and run them yourself.
#
# Usage:
#   bash tools/env_check.sh                 # writes env_report.txt in the current dir
#   bash tools/env_check.sh my_report.txt   # custom report path
# =============================================================================

set -u   # treat unset variables as errors (we intentionally do NOT use -e:
         # a missing tool must be reported, not abort the whole inspection)

REPORT="${1:-env_report.txt}"

# Send everything both to the terminal and to the report file.
exec > >(tee "$REPORT") 2>&1

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

# section "Title" — prints a visible header so the report is easy to scan.
section() {
    printf '\n==================== %s ====================\n' "$1"
}

# run "command ..." — prints the exact command, then runs it.
# If the program is not installed, says so instead of failing.
run() {
    local program="${1%% *}"
    printf '\n$ %s\n' "$*"
    if ! command -v "$program" >/dev/null 2>&1; then
        echo "  -> NOT INSTALLED: $program"
        return 0
    fi
    bash -c "$*" 2>&1 | sed 's/^/  /'
}

# have "program" — returns success if the program exists in PATH.
have() { command -v "$1" >/dev/null 2>&1; }

# -----------------------------------------------------------------------------
echo "Virtual Camera Engine — environment report"
echo "Generated: $(date -Iseconds)"

section "1. Operating system and kernel"
run "cat /etc/os-release"
run "uname -a"
run "nproc"
run "free -h"
run "cat /sys/devices/system/clocksource/clocksource0/current_clocksource"
echo
echo "Kernel preemption model (PREEMPT / PREEMPT_DYNAMIC / PREEMPT_RT):"
uname -v | sed 's/^/  /'
if [ -r /sys/kernel/debug/sched/preempt ]; then
    sed 's/^/  /' /sys/kernel/debug/sched/preempt
fi

section "2. Build toolchain"
run "g++ --version"
run "clang++ --version"
run "cmake --version"
run "make --version"
run "pkg-config --version"

section "3. FFmpeg development libraries (needed to BUILD the engine)"
for lib in libavformat libavcodec libavutil libswscale; do
    if have pkg-config && pkg-config --exists "$lib"; then
        printf '  %-12s %s\n' "$lib" "$(pkg-config --modversion "$lib")"
    else
        printf '  %-12s MISSING (install %s-dev)\n' "$lib" "$lib"
    fi
done
run "ffmpeg -hide_banner -version"
run "ffprobe -hide_banner -version"
echo
echo "FFmpeg v4l2 output support (used only for the smoke test):"
if have ffmpeg; then
    ffmpeg -hide_banner -devices 2>/dev/null | grep -i v4l2 | sed 's/^/  /' || echo "  (none listed)"
fi

section "4. Optional libraries"
if have pkg-config && pkg-config --exists opencv4; then
    echo "  opencv4 (C++)  $(pkg-config --modversion opencv4)"
else
    echo "  opencv4 (C++)  not found (optional)"
fi
if have python3; then
    python3 -c "import cv2; print('  OpenCV (Python)', cv2.__version__)" 2>/dev/null \
        || echo "  OpenCV (Python) not found (optional, used for consumer tests)"
    python3 -c "import numpy; print('  NumPy', numpy.__version__)" 2>/dev/null \
        || echo "  NumPy not found (optional)"
fi
for lib in yaml-cpp gtest; do
    if have pkg-config && pkg-config --exists "$lib"; then
        printf '  %-12s %s\n' "$lib" "$(pkg-config --modversion "$lib")"
    else
        printf '  %-12s not found (optional)\n' "$lib"
    fi
done
run "gst-inspect-1.0 --version"
if have gst-inspect-1.0; then
    echo
    echo "GStreamer v4l2sink element:"
    gst-inspect-1.0 v4l2sink >/dev/null 2>&1 && echo "  present" || echo "  not present"
fi

section "5. V4L2 devices"
run "ls -l /dev/video*"
run "v4l2-ctl --list-devices"
if have v4l2-ctl; then
    for dev in /dev/video*; do
        [ -e "$dev" ] || continue
        echo
        echo "---- $dev ----"
        v4l2-ctl -d "$dev" --info 2>&1 | sed 's/^/  /'
        v4l2-ctl -d "$dev" --list-formats-ext 2>&1 | head -40 | sed 's/^/  /'
    done
fi

section "6. v4l2loopback"
echo "Loaded right now?"
if lsmod | grep -q '^v4l2loopback'; then
    echo "  YES"
    lsmod | grep '^v4l2loopback' | sed 's/^/  /'
    [ -r /sys/module/v4l2loopback/version ] && \
        echo "  module version: $(cat /sys/module/v4l2loopback/version)"
    echo "  current parameters:"
    for p in /sys/module/v4l2loopback/parameters/*; do
        [ -r "$p" ] && printf '    %-20s %s\n' "$(basename "$p")" "$(cat "$p" 2>/dev/null)"
    done
else
    echo "  NO"
fi
echo
echo "Installed on disk?"
run "modinfo v4l2loopback"
if have dpkg; then
    echo
    echo "Debian packages:"
    # Store the matches first so we can print a clear message when there are none.
    packages=$(dpkg -l 2>/dev/null | grep -E "v4l2loopback|v4l-utils|linux-headers-$(uname -r)")
    if [ -n "$packages" ]; then
        echo "$packages" | sed 's/^/  /'
    else
        echo "  (none of v4l2loopback / v4l-utils / matching linux-headers installed)"
    fi
fi
run "v4l2loopback-ctl --version"
[ -e /dev/v4l2loopback ] && echo "  /dev/v4l2loopback control device present (runtime device creation possible)"

section "7. Secure Boot (affects loading DKMS kernel modules)"
run "mokutil --sb-state"

section "8. Permissions and real-time limits"
echo "  user:   $(id -un)"
echo "  groups: $(id -Gn)"
if id -Gn | tr ' ' '\n' | grep -qx video; then
    echo "  -> member of 'video' group: YES"
else
    echo "  -> member of 'video' group: NO (may need: sudo usermod -aG video \$USER, then re-login)"
fi
echo "  max real-time priority (ulimit -r): $(ulimit -r)"
echo "  max locked memory     (ulimit -l): $(ulimit -l)"
echo "  CPU frequency governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'n/a')"

section "9. Desktop / camera stack (for browser compatibility later)"
echo "  XDG_SESSION_TYPE: ${XDG_SESSION_TYPE:-unknown}"
run "pipewire --version"
for app in google-chrome chromium chromium-browser firefox vlc obs ffplay; do
    if have "$app"; then printf '  %-18s installed\n' "$app"; else printf '  %-18s not found\n' "$app"; fi
done

# -----------------------------------------------------------------------------
section "10. Commands to run YOURSELF (need sudo — NOT executed by this script)"
cat <<'EOF'
  # Install everything on any common distribution (apt / dnf / pacman / zypper):
  bash tools/install_deps.sh

  # Or by hand on Ubuntu/Debian:
  sudo apt update
  sudo apt install -y build-essential cmake pkg-config \
       libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
       ffmpeg v4l-utils v4l2loopback-dkms v4l2loopback-utils \
       linux-headers-$(uname -r)
  # Optional (tests / config / consumer checks):
  sudo apt install -y libgtest-dev libyaml-cpp-dev python3-opencv

  # Create the virtual camera device /dev/video10:
  sudo modprobe v4l2loopback devices=1 video_nr=10 \
       card_label="Virtual Camera Engine" exclusive_caps=1 max_buffers=2

  # Minimal virtual-camera smoke test (terminal 1 = producer):
  ffmpeg -hide_banner -re -f lavfi -i testsrc2=size=1280x720:rate=30 \
         -pix_fmt yuyv422 -f v4l2 /dev/video10

  # Terminal 2 = consumers:
  v4l2-ctl --all -d /dev/video10
  ffplay -hide_banner -f v4l2 /dev/video10
  python3 -c "import cv2; c=cv2.VideoCapture('/dev/video10', cv2.CAP_V4L2); ok,f=c.read(); print('read ok:',ok, 'shape:', None if f is None else f.shape, 'fps:', c.get(cv2.CAP_PROP_FPS))"

  # Remove the device when done:
  sudo modprobe -r v4l2loopback
EOF

echo
echo "Report written to: $REPORT"
