# 3. Debian rootfs

Build an arm64 Debian 12 rootfs and unpack it to `/data/linux` on the tablet:

```sh
docker run --platform linux/arm64 --name m8rootfs debian:bookworm-slim true
docker export m8rootfs > debian-arm64.tar
adb push debian-arm64.tar /data/local/tmp/
adb shell su -c 'mkdir -p /data/linux && tar -xpf /data/local/tmp/debian-arm64.tar -C /data/linux'
```

Then, in a chroot (`scripts/chroot.sh`, run as root from `adb shell`), install at least:

```sh
apt install --no-install-recommends openssh-server wpasupplicant udhcpc iproute2 python3 libinput-tools
```

**Wifi credentials:** Android keeps them in `/data/misc/apexdata/com.android.wifi/WifiConfigStore.xml`. Write a
`/etc/wpa_supplicant/wpa.conf` with the same SSID and passphrase. Put your ssh public key in
`/root/.ssh/authorized_keys` and use port 2222 (Android's adbd and other services stay on the usual ports).

Flags in the rootfs that the boot module reads:

| File | Effect |
|---|---|
| `/root/autostart-gpu` | GPU mode: bind binderfs, start the hwcomposer, run `/root/sway-start.sh` |
| `/root/autostart-media` | start the Android-side helpers for sound, camera and sensors (chapter 7) |
| `/root/android-once` | the next boot is Android (deleted on use) |
