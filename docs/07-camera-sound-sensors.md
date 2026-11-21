# 7. Camera, sound and sensors

All three only exist behind Android's HALs. The Android-side helpers in `android-side/` (built with
`NDK=... android-side/build.sh`, copied to `/data/adb/m8media`, chmod 700) use Android's own services, and
`android-start.sh` starts them in Linux mode (flag `/root/autostart-media`).

## Stand-ins for system_server: fakeperm

In Linux mode system_server doesn't run, and Android 12's native services wait for its binder services forever.
`common/fakeperm.c` registers stand-ins through libbinder_ndk. Each one fixes a concrete hang:

| Service | Without it |
|---|---|
| `activity` | audioserver never finishes starting its audio policy (`ActivityManager::getService()` loop) |
| `permission`, `permission_checker` | every permission check hangs: opening an audio stream, recording, setting a volume |
| `appops`, `sensor_privacy` | recording and camera access are refused or silenced |
| `scheduling_policy` | audioserver's FastMixer retries every second before each HAL write: silence, and `MonoPipe ... break monopipe write` in logcat |
| `package_native` | `MediaCodec::configure` waits for it: no H.264 encoder |
| `processinfo` | the camera service refuses every connect ("Priority score query failed: -110"); the reply needs one state and one score per pid |

They grant everything, so only run them in Linux mode (the Android UI and its apps are stopped).

## Sound

`m7audio` plays PulseAudio's output through **AAudio → audioserver**; PulseAudio itself (`services/audio/`) only has
a null sink. Never let anything open the ALSA card directly. Two M8 specifics:
* Volume: without system_server the stream volumes sit at -inf. `m7audio volume` initialises and sets **only the
  music stream**; initialising every stream made the MTK audio HAL hang on the next recording, and audioserver then
  aborts on a TimeCheck timeout (and forgets the volumes). Level 8 of 15 is too quiet on the M8's speakers; 13 is fine.
* **Never run `dumpsys media.audio_flinger`**: the dump takes longer than audioserver's TimeCheck allows, and it
  aborts and restarts, again without volumes.

## Camera

Android 12's own **cameraserver** works with fakeperm, plus one more thing system_server normally does: tell it that
user 0 is active. Otherwise every client gets "cannot connect from device user 0". `android-start.sh` sends
`ICameraService.notifySystemEvent(EVENT_USER_SWITCHED, [0])` (transaction 17 on Android 12, read from
`BpCameraService` in `libcamera_client.so`):

```sh
service call media.camera 17 i32 1 i32 1 i32 0
```

`m7cam` encodes with the Codec2 encoder `c2.mtk.avc.encoder` (~14 fps at 1280×720) and serves raw H.264 on
127.0.0.1:8093 to root clients only; `services/camera/m7cam-rtsp.py` turns that into an RTSP stream with a password.
The camera only runs while someone watches.

## Accelerometer and ambient light

The sensors sit behind MediaTek's sensor hub (SCP). `/sys/class/sensor/m_acc_misc` and `/dev/m_acc_misc` deliver
nothing on their own, but MediaTek's **legacy sensors HAL module** `sensors.mt6761.so` works when loaded directly by
`m7sensors`:
* The linker won't let a binary outside `/vendor` load from `/vendor`: copy the module and its vendor-only
  dependencies (`libksensor.so`, `libaedv.so`) to `/data/adb/m8media/vlib` and point `M7SENSORS_MODULE` and
  `LD_LIBRARY_PATH` there (`android-start.sh` does that).
* The module only builds its sensor list once the poll device is open.
* Stop Android's `vendor.sensors-hal-2-0-multihal` first: it reads the same devices.

`m7sensors stream` writes one JSON line per event into the FIFO `/run/m7sensors/stream` inside the chroot; create
the FIFO on the Linux side before starting `services/hw/tablethw.py`, which reads it.
