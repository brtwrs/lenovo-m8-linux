#!/system/bin/sh
# Decide, very early in boot, whether this boot becomes Linux or stays Android.
#  /data/linux/root/android-once  : one Android boot (set by Volume UP in linux-boot.sh, or `touch` it from Debian)
#  /data/adb/m8linux/linux-mode   : one Linux boot
#  /data/adb/m8linux/linux-vast   : always Linux. If the previous attempt never reached ssh
#                                   (linux-attempt still exists), this one boot falls back to Android.
# Flags and log live in /data/adb/m8linux (root only, 700), not in the shell-writable /data/local/tmp.
T=/data/adb/m8linux; mkdir -p $T; chmod 700 $T; A=/data/linux/root/android-once; P=$T/linux-attempt
[ -f $A ] && { rm -f $A $P; sync; exit 0; }
if [ -f $T/linux-mode ]; then rm -f $T/linux-mode
elif [ -f $T/linux-vast ]; then [ -f $P ] && { rm -f $P; sync; exit 0; }
else exit 0; fi
touch $P; sync
setsid sh ${0%/*}/linux-boot.sh </dev/null >/dev/null 2>&1 &
