# 1. Overview and requirements

**Device:** Lenovo Tab M8 4th gen, **TB300FU** (wifi). Tested firmware: Android 12, `S000099_230307_ROW`.
SoC MediaTek MT8766 (platform `mt6761`), 4× Cortex-A53, PowerVR GE8300 (GLES 3.2), 3 GB RAM, 800×1280 panel,
kernel 4.19 (A/B, slot `_a`, dynamic partitions, `vendor_boot`).

**What you get:** Debian 12 arm64 in a chroot on `/data/linux`, started very early in boot by a Magisk module, with
Sway on the GPU and your own app on top. Android keeps its kernel, init and vendor services; only its UI is stopped.
You can still boot Android (Volume UP during boot).

**Why not mainline / postmarketOS?** MT8766 has no usable mainline display or GPU support, and the bootloader only
boots Lenovo's kernel. The DRM device is only the PowerVR render node; the panel is behind `mtkfb` and the
hwcomposer. So, like the M7 project, everything graphical goes through Android's own drivers via libhybris.

**You need:**
- A Linux or macOS machine with `adb`, `fastboot`, Python 3 and [mtkclient](https://github.com/bkerler/mtkclient)
  (for the BROM backup).
- Magisk (tested 31.1), its arm64 tools for patching `boot_a` on the tablet.
- The Android NDK (r27c tested) for the Android-side helpers.
- Halium system images from LuneOS (arm64 Halium 13 and Halium 11) for bionic and the hwcomposer compat layer.
- A Debian 12 arm64 rootfs (Docker `debian:bookworm-slim --platform linux/arm64` is the easy way).

**Time:** a day if everything goes right. The BROM backup is the step that needs patience.
