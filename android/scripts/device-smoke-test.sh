#!/usr/bin/env bash
#
# On-device smoke test for the xemu Android APK.
#
# Runtime validation of the Vulkan Xbox emulator can't happen in CI (it
# needs a Vulkan-capable GPU and a legally-obtained MCPX/flash dump), so
# this script automates the on-device checks: install, launch, and assert
# the expected native/JNI/Vulkan/qemu markers appear in logcat.
#
# Usage:
#   android/scripts/device-smoke-test.sh path/to/app-debug.apk
#
# Optionally push BIOS files first so qemu_init actually runs:
#   adb push mcpx_1.0.bin  /data/local/tmp/      # then pick via launcher
#   adb push xbox_flash.bin /data/local/tmp/
#
# Exit codes:
#   0  all required markers seen
#   1  a required marker was missing (see the summary)
#   2  setup error (no device, install failed, etc.)

set -u

APK="${1:-}"
PKG="app.xemu"
LAUNCH_ACT="$PKG/.LauncherActivity"
EMU_ACT="$PKG/.EmulatorActivity"
TAGS="xemu xemu-vk xemu-jni"
CAPTURE_SECONDS="${CAPTURE_SECONDS:-25}"

fail() { echo "ERROR: $*" >&2; exit 2; }

[ -n "$APK" ] || fail "usage: $0 path/to/app-debug.apk"
[ -f "$APK" ] || fail "APK not found: $APK"
command -v adb >/dev/null || fail "adb not on PATH"

# --- device present? ---------------------------------------------------
if ! adb get-state >/dev/null 2>&1; then
    fail "no device/emulator connected (check 'adb devices')"
fi
DEVICE_MODEL=$(adb shell getprop ro.product.model | tr -d '\r')
echo "Device: ${DEVICE_MODEL:-unknown}"
VK_SUPPORT=$(adb shell pm list features 2>/dev/null | grep -c vulkan.version || true)
echo "Vulkan feature reported: ${VK_SUPPORT}"

# --- install -----------------------------------------------------------
echo "Installing $APK ..."
adb install -r -g "$APK" >/dev/null 2>&1 || adb install -r "$APK" || \
    fail "adb install failed"

# --- launch + capture --------------------------------------------------
adb logcat -c || true
echo "Launching $LAUNCH_ACT ..."
adb shell am start -n "$LAUNCH_ACT" >/dev/null || fail "could not start LauncherActivity"

# Give the user a moment to tap Start manually if no BIOS auto-config is
# present; if EmulatorActivity is started directly that's fine too.
sleep 3
adb shell am start -n "$EMU_ACT" >/dev/null 2>&1 || true

echo "Capturing logcat for ${CAPTURE_SECONDS}s (tags: $TAGS) ..."
LOG=$(mktemp)
# shellcheck disable=SC2086
timeout "${CAPTURE_SECONDS}" adb logcat -s $TAGS > "$LOG" 2>/dev/null || true

echo "----- captured log -----"
cat "$LOG"
echo "------------------------"

# --- assertions --------------------------------------------------------
# Required: the native lifecycle + Vulkan presenter must come up.
REQUIRED=(
  "xemu_android_start"          # JNI -> native entry reached
  "Created Android Vulkan surface|vkCreateAndroidSurface|Swapchain extent|shared: presenting|nv2a not present" # presenter engaged (either path)
)
# Informational: present only when BIOS+flash were configured.
OPTIONAL=(
  "qemu_main_thread: calling qemu_init"   # emulation actually started
  "registered xemu-android DCL"           # nv2a console came up
  "shared: presenting nv2a output"        # the real game-frame blit path
  "dpy_gfx_switch: new surface"           # nv2a produced a display surface
)

rc=0
echo
echo "=== required markers ==="
for pat in "${REQUIRED[@]}"; do
    if grep -Eq "$pat" "$LOG"; then
        echo "  [PASS] $pat"
    else
        echo "  [FAIL] $pat"
        rc=1
    fi
done

echo
echo "=== optional markers (present once BIOS+flash are configured) ==="
for pat in "${OPTIONAL[@]}"; do
    if grep -Eq "$pat" "$LOG"; then
        echo "  [seen] $pat"
    else
        echo "  [ -- ] $pat"
    fi
done

rm -f "$LOG"
echo
if [ "$rc" -eq 0 ]; then
    echo "RESULT: launch + native + presenter validated."
    echo "If the optional markers are missing, configure MCPX + flash in the"
    echo "launcher to exercise the full qemu_init -> nv2a -> blit path."
else
    echo "RESULT: a required marker was missing - see [FAIL] above and the log."
fi
exit "$rc"
