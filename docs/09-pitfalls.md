# 9. Recovery, pitfalls and debugging

## Getting back to Android
* Volume UP during the first 20 s of a Linux boot → one Android boot. From Debian: `touch /root/android-once && reboot`.
* Ended up in Android unexpectedly? Magisk's safe mode disables the module (Volume DOWN during boot, or the boot-loop
  counter on an older module). Re-enable `m8linux` in the Magisk app, or `su -c rm /data/adb/modules/m8linux/disable`.

## An Android shell without USB
From Debian as root: `chroot /proc/1/root /system/bin/sh -c "..."` runs a command in Android's root, with root.
Handy once the tablet hangs on a wall (wifi adb is off on a stock build).

## Things that cost time
* **Qt and `qtbase5-dev`**: installing it replaces the GLES Qt and breaks every Qt app (chapter 6).
* **Load average ~12–14 with an idle CPU**: about eleven MediaTek kernel threads (battery, charger, gauge, display
  watchdogs) sleep in state D, which Linux counts as load. Harmless, same in Android.
* **The IP address may change between boots** (DHCP); give the tablet a fixed lease in your router.
* **adb through a container** (LXC with USB passthrough): after a reboot the tablet gets a new USB device number
  and the container has no node for it. `mknod /dev/bus/usb/<bus>/<dev> c 189 <(bus-1)*128+dev-1>` instead of
  restarting the adb server (which drops your other devices).
* `pgrep -f`/`pkill -f` over ssh also match your own ssh command line. And a `/bin/sh script.sh` process is named
  after the script, not `sh`: look in `/proc/*/cmdline`.
* **No `dumpsys media.audio_flinger`** (chapter 7).
