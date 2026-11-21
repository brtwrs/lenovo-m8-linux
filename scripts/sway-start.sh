#!/bin/sh
# Start Sway on the Tab M8: wlroots' hwcomposer backend (Android's hwcomposer service) + the PowerVR GPU via libhybris.
# Called by linux-boot.sh (GPU mode). Needs /dev/binderfs bound into the chroot and vendor.hwcomposer-2-1 running.
export LD_LIBRARY_PATH=/opt/wlr/lib/aarch64-linux-gnu:/opt/hybris/lib
export HYBRIS_LD_LIBRARY_PATH=/opt/hybris/android/lib64:/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64:/opt/hybris/android/i18n
export WLR_BACKENDS=hwcomposer,libinput XDG_SEAT=seat1 EGL_PLATFORM=hwcomposer XDG_RUNTIME_DIR=/tmp/xdg LANG=C.UTF-8
mkdir -p /run/udev/data
# /tmp is world-writable: the runtime dir must be a real directory owned by root, never a symlink planted there
[ -L /tmp/xdg ] && { echo "/tmp/xdg is a symlink, refusing" >&2; exit 1; }
install -d -m 700 -o root -g root /tmp/xdg
[ "$(stat -c %u:%a /tmp/xdg)" = "0:700" ] || { echo "/tmp/xdg is not root's 0700 directory, refusing" >&2; exit 1; }
# There is no udev in the chroot: give libinput the udev data it needs. The real touchscreen is fts_ts;
# mtk-tpd exists but never reports. Both get the data, it doesn't hurt.
for e in /sys/class/input/event*; do case "$(cat $e/device/name)" in mtk-tpd|fts_ts)
  printf "E:ID_INPUT=1\nE:ID_INPUT_TOUCHSCREEN=1\nE:ID_SEAT=seat1\nG:seat\n" > /run/udev/data/c$(cat $e/dev);; esac; done
setsid /opt/wlr/bin/sway -d </dev/null >/run/sway.log 2>&1 &   # log in root-only /run, not in /tmp
