#!/system/bin/sh
# Linux mode: stop the Android UI, bring up wifi from Debian, start sshd on :2222, then the screen stack.
# Log: /data/adb/m8linux/m8linux.log (root-only directory, see post-fs-data.sh)
MOD=${0%/*}; R=/data/linux; T=/data/adb/m8linux; L=$T/m8linux.log
mkdir -p $T; chmod 700 $T; [ -L $L ] && rm -f $L
exec >>$L 2>&1; echo "== $(date) linux-boot"; log(){ echo "$(date +%T) $*"; sync; }
P=$T/linux-attempt; A=$R/root/android-once
# Safety net: no ssh after 5 minutes -> reboot; post-fs-data then sees linux-attempt and boots Android once.
( sleep 300; [ -f $T/linux-stay ] || [ ! -f $P ] || { log "safety net: no ssh, reboot"; reboot; } ) &
# Escape hatch: Volume UP in the first 20 s -> one Android boot.
# NOT Volume DOWN: that is Magisk's safe mode, which disables this module (re-enable it in the Magisk app).
( timeout 20 getevent -lq 2>/dev/null | grep -m1 -q "KEY_VOLUMEUP *DOWN" && { log "Vol+: Android once"; touch $A; sync; reboot; } ) &
# GPU mode (/root/autostart-gpu in Debian): Android's hwcomposer keeps running, the compositor draws via libhybris.
GPU=; [ -f $R/root/autostart-gpu ] && GPU=1
STOP="bootanim zygote zygote_secondary surfaceflinger"; [ -z "$GPU" ] && STOP="$STOP vendor.hwcomposer-2-1"
# 1. Keep stopping the Android UI for 60 s (init restarts it via class_start otherwise).
for i in $(seq 120); do
  for s in $STOP; do [ "$(getprop init.svc.$s)" = running ] && { stop $s; log "stop $s"; }; done; sleep 0.5
done &
# 2. Backlight on (0..255). The M8 draws nothing on fb0 itself: the screen stays dark until Sway runs.
sleep 8
echo 160 > /sys/class/leds/lcd-backlight/brightness
# 3. Wifi chip on. Modules + firmware are loaded by Android init already (class core).
for i in $(seq 60); do [ "$(getprop vendor.connsys.driver.ready)" = yes ] && [ -e /dev/wmtWifi ] && break; sleep 1; done
log "connsys.ready=$(getprop vendor.connsys.driver.ready) after ${i}s"
echo 1 > /dev/wmtWifi; sleep 1; echo STA > /dev/wmtWifi
for i in $(seq 30); do [ -e /sys/class/net/wlan0 ] && break; sleep 0.5; done; log "wlan0: $(cat /sys/class/net/wlan0/operstate 2>&1)"
# 4. Debian: mounts, wifi, dhcp, sshd
grep -q " $R/proc " /proc/mounts || {
  mount -t proc proc $R/proc; mount -t sysfs sys $R/sys; mount -o bind /dev $R/dev
  mount -t devpts devpts $R/dev/pts 2>/dev/null || mount -o bind /dev/pts $R/dev/pts
  mount -t tmpfs tmp $R/tmp; mkdir -p /dev/shm; mount -t tmpfs -o mode=1777 shm /dev/shm; mount -t tmpfs -o mode=755 run $R/run
  mkdir -p $R/vendor $R/system; mount -o bind /vendor $R/vendor; mount -o bind /system $R/system
}
ip rule add from all lookup main pref 9999 2>/dev/null      # Android policy routing would otherwise hide the main table
unset LD_PRELOAD TMPDIR ANDROID_DATA ANDROID_ROOT BOOTCLASSPATH
C(){ /data/adb/magisk/busybox chroot $R /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root "$@"; }
C /bin/sh -c 'ip link set wlan0 up; wpa_supplicant -B -i wlan0 -D nl80211 -c /etc/wpa_supplicant/wpa.conf -f /tmp/wpa.log'
for i in $(seq 30); do C /sbin/wpa_cli -i wlan0 status 2>/dev/null | grep -q wpa_state=COMPLETED && break; sleep 1; done
log "wpa: $(C /sbin/wpa_cli -i wlan0 status 2>&1 | grep -E 'wpa_state|ssid=' | tr '\n' ' ')"
C /bin/sh -c 'udhcpc -i wlan0 -n -q -t 10 -s /etc/udhcpc/default.script'; log "ip: $(ip -4 addr show wlan0 | grep inet)"
C /bin/sh -c 'mkdir -p /run/sshd; chmod 755 /run /run/sshd; /usr/sbin/sshd -p 2222'; log "sshd rc=$?"; rm -f $P; sync
# Magisk counts every boot as failed until Android reports boot-complete (magisk.db, settings.bootloop),
# which never happens in Linux mode. After two Linux boots in a row, the third would start in Magisk's
# safe mode, which disables every module (so: Android). Linux only counts as a success here (ssh is up),
# so reset the counter ourselves, as Magisk does at boot-complete. A Linux boot that hangs still counts.
magisk --sqlite "REPLACE INTO settings (key,value) VALUES('bootloop',0)"; log "magisk boot counter reset"
# 5a. GPU mode: libhybris needs Android's binderfs inside the chroot, and the hwcomposer service back on.
#     sway-start.sh starts Sway; Sway's config starts your app (scripts/sway-config).
if [ -n "$GPU" ]; then
  mkdir -p $R/dev/binderfs; grep -q " $R/dev/binderfs " /proc/mounts || mount -o bind /dev/binderfs $R/dev/binderfs
  start vendor.hwcomposer-2-1; sleep 3
  C /bin/sh /root/sway-start.sh; log "Sway started"
fi
# 5b. No GPU: X11 + i3 on the framebuffer (software rendering), if /root/autostart-x exists
[ -z "$GPU" ] && [ -f $R/root/autostart-x ] && { C /bin/sh -c "cd /root; LANG=C.UTF-8 setsid xinit -- :0 -nolisten tcp -nocursor -keeptty -novtswitch -sharevts </dev/null >/tmp/xinit.log 2>&1 &"; log "X started"; }
# 5c. Sound, camera and sensors (/root/autostart-media): Android-side helpers in /data/adb/m8media
#     (docs/07-camera-sound-sensors.md). Their Linux-side counterparts start with your app's start script.
[ -f $R/root/autostart-media ] && [ -x /data/adb/m8media/android-start.sh ] && { setsid sh /data/adb/m8media/android-start.sh </dev/null >/dev/null 2>&1 & log "media started"; }
