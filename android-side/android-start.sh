#!/system/bin/sh
# Android side of sound, camera and sensors on the Tab M8 (TB300FU, Android 12) in Linux mode.
# Started by linux-boot.sh when /root/autostart-media exists in the Debian rootfs. Everything lives in $D:
#   fakeperm, m7audio, m7cam, m7sensors   (android-side/build.sh)
#   vlib/                                 (copies of the vendor sensors HAL module, see below)
#   volume   (optional: 0..15, default 13; 8 is too quiet on the M8)
#   camera   (optional: 0 = back, 1 = front; default 1)
D=/data/adb/m8media; cd $D || exit 1     # root only (chmod 700): these binaries run as root
exec >>$D/start.log 2>&1; echo "== $(date) android-start"

# Stand-ins for system_server's services. Without them audioserver waits for "activity" while starting its audio
# policy, every permission check hangs, the FastMixer never writes (scheduling_policy), MediaCodec waits for
# package_native and the camera service refuses connections (processinfo). See docs/07.
( while true; do ./fakeperm; echo "$(date +%T) fakeperm stopped"; sleep 3; done ) &
for i in $(seq 90); do service check media.audio_policy | grep -q found && break; sleep 1; done
echo "audio_policy after ${i}s"; sleep 2

# Sound: only the music stream volume (initialising every stream made the MTK HAL hang), then PulseAudio's
# null-sink monitor (unix socket inside the chroot) through audioserver.
./m7audio volume $(cat $D/volume 2>/dev/null || echo 13)
( while true; do ./m7audio play /data/linux/run/m7pulse/pcm </dev/null >>$D/m7audio.log 2>&1; echo "$(date +%T) m7audio stopped"; sleep 3; done ) &

# Camera: Android's own cameraserver works with fakeperm, but refuses clients until it has been told that user 0
# is active (system_server normally does that): ICameraService.notifySystemEvent(EVENT_USER_SWITCHED=1, [0]),
# transaction 17 on Android 12 (read from BpCameraService in libcamera_client.so).
for i in $(seq 30); do service check media.camera | grep -q found && break; sleep 1; done
service call media.camera 17 i32 1 i32 1 i32 0 >/dev/null
CAM=$(cat $D/camera 2>/dev/null || echo 1)
( while true; do ./m7cam $CAM 8093 1280 720 </dev/null >>$D/m7cam.log 2>&1; echo "$(date +%T) m7cam stopped"; sleep 3; done ) &

# Sensors (accelerometer, light): MediaTek's legacy sensors HAL module, copied to vlib/ because the linker won't let
# a binary outside /vendor load from /vendor. Android's own sensors HAL service reads the same devices and nobody
# uses it in Linux mode: stop it. m7sensors writes JSON lines into the pipe the Linux side creates.
mkdir -p $D/vlib
for f in /vendor/lib64/hw/sensors.*.so /vendor/lib64/libksensor.so /vendor/lib64/libaedv.so; do
  [ -f $D/vlib/${f##*/} ] || cp $f $D/vlib/
done
stop vendor.sensors-hal-2-0-multihal
P=/data/linux/run/m7sensors/stream
MODULE=$(ls $D/vlib/sensors.*.so | head -1)
( while true; do
    if [ -p $P ]; then M7SENSORS_MODULE=$MODULE LD_LIBRARY_PATH=$D/vlib ./m7sensors stream >$P 2>>$D/sensors.log; fi
    sleep 3
  done ) &
echo started
