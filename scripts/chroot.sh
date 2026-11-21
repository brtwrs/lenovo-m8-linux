#!/system/bin/sh
# Enter the Debian chroot from Android (adb shell + su), e.g. while Android is still running.
# Usage: sh chroot.sh [command...]
R=/data/linux; B=/data/adb/magisk/busybox
grep -q " $R/proc " /proc/mounts || {
  mount -t proc proc $R/proc; mount -t sysfs sys $R/sys
  mount -o bind /dev $R/dev; mount -t devpts devpts $R/dev/pts 2>/dev/null || mount -o bind /dev/pts $R/dev/pts
  mount -t tmpfs tmp $R/tmp; mkdir -p $R/vendor $R/system; mount -o bind /vendor $R/vendor; mount -o bind /system $R/system
}
echo "nameserver ${DNS:-1.1.1.1}" > $R/etc/resolv.conf
echo 'APT::Sandbox::User "root";' > $R/etc/apt/apt.conf.d/01-android     # no _apt user/namespaces on this kernel
ip rule add from all lookup main pref 9999 2>/dev/null
unset LD_PRELOAD TMPDIR ANDROID_DATA ANDROID_ROOT BOOTCLASSPATH
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin HOME=/root TERM=${TERM:-linux}
exec $B chroot $R "${@:-/bin/bash}"
