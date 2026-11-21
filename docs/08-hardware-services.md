# 8. Buttons, standby, sensors and Bluetooth for Home Assistant

## tablethw.py

`services/hw/tablethw.py` runs on the Linux side (it is the same file as in the M7 project and detects the tablet):

* Power: standby on/off. The M8 reports every press **twice**, about a second apart; the second one is ignored.
* Volume buttons: PulseAudio volume ±6 % with a click; in standby they only wake the screen.
* Tap to wake. Automatic standby after `standby_after` seconds without touch or button, unless audio plays.
* **Standby on the M8 = `standby_mode: "black"`**: the backlight chip (sgm37604a) still glows at brightness 0, and
  Sway's `output * dpms off` really turns the panel off but also puts the touchscreen to sleep, so tap-to-wake stops
  working. "black" keeps the panel on with backlight 0 and expects your app to draw black while
  `GET http://127.0.0.1:8091/status` reports `"standby": true`.
* MQTT with Home Assistant discovery: screen, vibrate, button events, volume, battery, temperatures, orientation,
  tilt, motion, illuminance (the M8 has no LEDs and no proximity sensor).

```sh
apt install --no-install-recommends python3-evdev python3-paho-mqtt gstreamer1.0-tools gstreamer1.0-pulseaudio
mkdir -p /root/tablethw && cp services/hw/tablethw.py /root/tablethw/
cp services/hw/config.example.json /root/tablethw/config.json && chmod 600 /root/tablethw/config.json   # edit it
mkdir -p /run/m7sensors && chmod 700 /run/m7sensors && mkfifo -m 600 /run/m7sensors/stream
setsid python3 /root/tablethw/tablethw.py </dev/null >/tmp/tablethw.log 2>&1 &
```

## Bluetooth proxy

`/dev/stpbt` is a raw H4 HCI device, the same as on the M7; Android's Bluetooth HAL does not hold it open in Linux
mode. `services/bluetooth/bt-proxy.py` scans passively and acts as an ESPHome Bluetooth proxy for Home Assistant;
put your Home Assistant IP in `bt.json` → `allowed` (see the comments at the top of the file).

## Sound and camera on the Linux side

* PulseAudio: `services/audio/m7.pa` and `pulse-start.sh`, `/etc/asound.conf` → pulse (see the M7 project's
  chapter 13; identical).
* Camera: `services/camera/m7cam-rtsp.py` with a `cam.json` next to it (`user`, `password`, `port`).
