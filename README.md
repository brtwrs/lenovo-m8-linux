# Debian on the Lenovo Tab M8 (4th gen, TB300FU), with the GPU

Run a real Debian 12 (arm64) on the Lenovo Tab M8 4th gen (TB300FU, MediaTek MT8766 / "mt6761", PowerVR GE8300,
3 GB RAM, Android 12), with wifi, touch, a **GPU-accelerated Wayland compositor (Sway)**, sound, cameras and sensors,
so it can drive a smooth wall dashboard without the Android UI.

Like its sibling projects [lenovo-m7-linux](https://github.com/brtwrs/lenovo-m7-linux) and
[lenovo-m11-linux](https://github.com/brtwrs/lenovo-m11-linux) (same setup on Android 13 with a Mali GPU), this is not a mainline port.
The stock Lenovo kernel (4.19) and Android's vendor services keep running, and Debian runs next to them in a chroot
that takes over the screen. The GPU is used through [libhybris](https://github.com/libhybris/libhybris) and Android's
own hwcomposer, the way Halium/Droidian phones do it. Camera, sound and sensors stay Android things too: small NDK
helpers on the Android side hand them over to Linux.

> ⚠️ **Read [docs/02-unlock-and-root.md](docs/02-unlock-and-root.md) first.** Unlocking the bootloader **wipes the
> tablet**. Make the BROM backup before you change anything. You do this at your own risk.

## What works

| | Status | Notes |
|---|---|---|
| Screen, backlight | ✅ | through Android's hwcomposer |
| **GPU** (PowerVR GE8300, GLES 3.2) | ✅ | libhybris (Halium 13 arm64 bionic + Halium 11 hwcomposer compat) + Droidian wlroots + **Sway 1.6.1**; Qt Quick at 60 fps |
| Touch | ✅ | `fts_ts` through libinput (fake udev data); Sway rotates touch itself, no calibration matrix |
| Wifi | ✅ | MediaTek combo chip via `/dev/wmtWifi`, Debian's `wpa_supplicant` |
| Battery, charging, thermal | ✅ | `/sys/class/power_supply/battery`, `mtktscpu` |
| Buttons (Vol−, Vol+, Power) | ✅ | on two input devices (`mtk-kpd` = Vol+, `mtk-pmic-keys` = Vol− and Power); Power reports each press twice |
| Screen standby, tap to wake | ✅ | backlight 0 + black screen (the backlight chip glows at 0; `dpms off` would also stop the touchscreen) |
| **Sound** (speaker, microphone) | ✅ | through Android's audioserver (AAudio) and PulseAudio; needs stand-ins for system_server services ([chapter 7](docs/07-camera-sound-sensors.md)) |
| **Cameras** (front, back) | ✅ | Android's cameraserver + Camera2 NDK + hardware H.264 encoder (Codec2) → RTSP, 1280×720 ~14 fps |
| **Accelerometer, ambient light** | ✅ | MediaTek's legacy sensors HAL module, loaded directly; orientation, tilt, motion, lux |
| Bluetooth (BLE) | ✅ | raw HCI on `/dev/stpbt`; ESPHome Bluetooth proxy for Home Assistant |
| Vibrator | ✅ | `/sys/class/leds/vibrator` |
| Proximity sensor | ⚪ not fitted | the HAL lists none (it does list tilt, wake gesture, device orientation and SAR) |
| Notification LEDs | ⚪ not fitted | |

## How it fits together

```
 ┌──────────────── Lenovo kernel 4.19 (stock), Android 12, Magisk in boot_a ─────────────────────────────────┐
 │ Android init (PID 1)                         Debian 12 arm64 chroot in /data/linux                         │
 │ ├─ vendor services: hwcomposer, audio HAL,   ├─ wpa_supplicant + udhcpc → wifi, sshd :2222                 │
 │ │  camera provider, sensors (stopped)        ├─ Sway (wlroots hwcomposer backend) ── your app (Qt Quick)   │
 │ ├─ audioserver, cameraserver                 │     └─ libhybris ── Halium bionic ── /vendor PowerVR blob    │
 │ ├─ zygote / system_server / UI: STOPPED      ├─ PulseAudio (null sink) ─┐                                  │
 │ ├─ fakeperm: stand-ins for system_server ◄───┼──────────────────────────┼─ m7audio (AAudio → audioserver)  │
 │ └─ m7cam (Camera2 → H.264) ── tcp ──────────►├─ RTSP server             │  m7sensors (sensors HAL) → pipe   │
 │    Magisk module m8linux: post-fs-data.sh → linux-boot.sh            tablethw.py (buttons, standby, MQTT)  │
 └────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

## Getting started

1. [Overview and requirements](docs/01-overview.md)
2. [Unlock the bootloader and root (Magisk in boot_a)](docs/02-unlock-and-root.md)
3. [Build and install the Debian rootfs](docs/03-debian-rootfs.md)
4. [Install the boot module: Linux by default, Android on request](docs/04-boot-module.md)
5. [GPU: libhybris with Halium bionic and the hwcomposer compat layer](docs/05-gpu-libhybris.md)
6. [Compositor: Droidian wlroots + Sway 1.6.1, touch, Qt](docs/06-sway.md)
7. [Camera, sound and sensors through Android's own services](docs/07-camera-sound-sensors.md)
8. [Buttons, standby, LEDs, sensors and Bluetooth for Home Assistant](docs/08-hardware-services.md)
9. [Recovery, pitfalls and debugging](docs/09-pitfalls.md)

## Repository layout

| Path | What |
|---|---|
| `magisk-module/m8linux/` | the boot module (`post-fs-data.sh`, `linux-boot.sh`) |
| `patches/` | Sway 1.6.1 against Droidian's wlroots, wlroots for PowerVR/MTK, compat shims |
| `shim/` | `libm8shim.so`: Android 11 symbols Halium 11's hwcomposer compat layer needs on Android 12 |
| `scripts/` | Sway start script, example Sway config, libhybris environment |
| `android-side/` | NDK helpers: `fakeperm` (service stand-ins), `m7audio`, `m7cam`, `m7sensors`, start script, `build.sh` |
| `services/` | Linux side: `hw/tablethw.py` (buttons, standby, sensors, MQTT), `audio/` (PulseAudio), `camera/` (RTSP), `bluetooth/` (BLE proxy) |

The `m7*` names come from the Tab M7 project; the same code runs on both tablets (`ARCH=arm64` builds for the M8).
No binaries from Lenovo, MediaTek or Halium are included; the docs show where to get them.

## Status of this repo

Everything here runs on a real TB300FU (firmware S000099_230307_ROW) as a wall dashboard. The build steps were
reconstructed from that working setup; if a step fails for you, please open an issue with the log.

## License

The scripts, docs and code in this repository are MIT licensed (see [LICENSE](LICENSE)). The patches in `patches/`
modify third-party projects and fall under their licenses: wlroots (MIT), Sway (MIT).
