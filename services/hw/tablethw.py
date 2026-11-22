#!/usr/bin/env python3
"""tablethw: hardware service for Lenovo tablets running Linux next to Android's vendor stack.
Tested on the Tab M7 (TB-7305F, Android 9) and the Tab M8 4th gen (TB300FU, Android 12). One file for both.

It detects what the tablet has:
  buttons      on one or two input devices (M7: mtk-kpd; M8: mtk-kpd = Vol+, mtk-pmic-keys = Vol- and Power)
  touchscreen  fts_ts (M8; only ABS_MT_TRACKING_ID) or mtk-tpd (M7; BTN_TOUCH)
  LEDs         /sys/class/leds/{red,green} (M7 only)
  motion       /dev/gsensor ioctl (M7) or the sensor pipe /run/m7sensors/stream (M8, fed by m7sensors on the
               Android side, which also delivers ambient light)

What it does:
  - Power button: screen standby on/off (long press is only reported). Volume buttons: PulseAudio volume ±6 %
    with a short click; in standby they only wake the screen. A tap wakes the screen too (the touchscreen is
    grabbed during standby, so that tap doesn't reach the app).
  - Automatic standby after `standby_after` seconds (default 300) without touch or button, unless audio plays.
  - Standby mode `standby_mode`: "dpms" (backlight 0, plus `swaymsg output * dpms off` when Sway runs) or
    "black" (backlight 0 only; your app draws black while GET /status says standby). On the M8 the backlight
    chip keeps glowing at 0, and dpms off also puts the touch controller to sleep, so tap-to-wake needs "black".
  - MQTT with Home Assistant discovery: screen (light), red/green LED (light with blink effects), vibrate button,
    button events, battery/charging/power/temperatures, orientation/tilt/motion, ambient light, volume.
  - Local HTTP on 127.0.0.1:8091: GET /status, POST /wake, POST /vibrate?ms=40.

config.json next to this file (chmod 600):
  {"mqtt": {"host": "192.168.1.10", "port": 1883, "user": "...", "pass": "..."},
   "id": "m8", "name": "Tablet M8", "model": "Lenovo Tab M8 (TB300FU)",
   "standby_after": 300, "standby_mode": "dpms",
   "pulse_server": "unix:/run/m7pulse/native", "pulse_sink": "m7", "pulse_user": null}
`pulse_user`: run the PulseAudio clients (pactl, the click) as this unprivileged user instead of root; give that user
access to the PulseAudio client socket. Leave it null if only root uses PulseAudio.
"""
import fcntl, glob, json, math, os, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
import evdev
import paho.mqtt.client as mqtt

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = json.load(open(os.path.join(HERE, "config.json")))
ID = CFG.get("id", "tablet")
NAME = CFG.get("name", "Tablet")
MODEL = CFG.get("model", "Lenovo tablet")
BASE = f"tablethw/{ID}"
LEDS = "/sys/class/leds/"
HAS_LEDS = os.path.exists(LEDS + "red") and os.path.exists(LEDS + "green")
BUTTON_DEVICES = ("mtk-kpd", "mtk-pmic-keys")
TOUCH_DEVICES = ("fts_ts", "himax-touchscreen", "mtk-tpd")   # prefer fts_ts: the M8 also has an mtk-tpd that never reports; M11 = Himax
STATE_FILE = os.path.join(HERE, "state.json")
SENSOR_PIPE = "/run/m7sensors/stream"


def read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def write(path, value):
    try:
        with open(path, "w") as f:
            f.write(str(value))
    except OSError as e:
        print("write", path, e, flush=True)


class Hw:
    def __init__(self):
        try:
            s = json.load(open(STATE_FILE))
        except (OSError, ValueError):
            s = {}
        self.brightness = int(s.get("brightness", 160))
        self.standby = bool(s.get("standby", False))      # persisted: a service restart must not turn the screen on
        self.leds = {"red": {"on": False, "effect": None}, "green": {"on": False, "effect": None}}
        self.lock = threading.Lock()
        self.mq = None
        self.touch = None
        self.touch_grabbed = False
        self.wake_tap = False
        self.last_activity = time.time()
        self.last_power = 0.0
        self.sensor_state = {}
        self.has_acc, self.acc_source = False, None
        self.pulse_env = {**os.environ, "PULSE_SERVER": CFG.get("pulse_server", "unix:/run/m7pulse/native")}
        user = CFG.get("pulse_user")
        self.as_pulse_user = []
        if user:
            self.as_pulse_user = ["setpriv", f"--reuid={user}", f"--regid={user}", "--init-groups",
                                  "--inh-caps=-all", "--no-new-privs", "--"]
            import pwd
            self.pulse_env["HOME"] = pwd.getpwnam(user).pw_dir   # never give a root process this user's HOME

    # ---------- screen ----------
    def save(self):
        try:
            json.dump({"brightness": self.brightness, "standby": self.standby}, open(STATE_FILE, "w"))
        except OSError:
            pass

    def set_screen(self, on, brightness=None, source=""):
        with self.lock:
            if brightness is not None:
                self.brightness = max(5, min(255, int(brightness)))
            self.standby = not on
            if on:
                self.last_activity = time.time()
            self.save()
            if on:
                self.dpms(True)
            write(LEDS + "lcd-backlight/brightness", self.brightness if on else 0)
            if not on:
                self.dpms(False)
            self.grab_touch(not on)
        print(f"screen {'on' if on else 'standby'} brightness={self.brightness} ({source})", flush=True)
        self.report_screen()

    def dpms(self, on):
        if CFG.get("standby_mode", "dpms") != "dpms":
            return
        sock = glob.glob("/tmp/xdg/sway-ipc.*.sock")
        if not sock:
            return                                           # no Sway (e.g. cage on the M7): the backlight is enough
        env = {**os.environ, "SWAYSOCK": sock[0], "LD_LIBRARY_PATH": "/opt/wlr/lib/aarch64-linux-gnu"}
        try:
            subprocess.run(["/opt/wlr/bin/swaymsg", f"output * dpms {'on' if on else 'off'}"], env=env,
                           capture_output=True, timeout=5)
        except (OSError, subprocess.SubprocessError) as e:
            print("dpms:", repr(e), flush=True)

    def grab_touch(self, grab):
        if not self.touch or grab == self.touch_grabbed:
            return
        try:
            (self.touch.grab if grab else self.touch.ungrab)()
            self.touch_grabbed = grab
        except OSError as e:
            print("grab", e, flush=True)

    # ---------- LEDs and vibrator ----------
    def set_led(self, colour, on, effect=None):
        d = LEDS + colour + "/"
        if not on:
            write(d + "trigger", "none"); write(d + "brightness", 0)
        elif effect in ("blink", "fast"):
            write(d + "trigger", "timer")
            t = 500 if effect == "blink" else 150
            write(d + "delay_on", t); write(d + "delay_off", t)
        else:
            write(d + "trigger", "none"); write(d + "brightness", 255)
        self.leds[colour] = {"on": on, "effect": effect if on else None}
        self.publish(f"led_{colour}", {"state": "ON" if on else "OFF", "effect": effect or "solid"})

    def vibrate(self, ms=60):
        ms = max(10, min(2000, int(ms)))
        write(LEDS + "vibrator/duration", ms)
        write(LEDS + "vibrator/activate", 1)

    # ---------- sound (PulseAudio only: never open the ALSA card directly, mmap crashes these kernels) ----------
    def pactl(self, *args):
        return subprocess.run(self.as_pulse_user + ["pactl", *args], capture_output=True, text=True, timeout=3,
                              env=self.pulse_env).stdout

    def change_volume(self, step):
        sink = CFG.get("pulse_sink", "m7")
        try:
            now = int(self.pactl("get-sink-volume", sink).split("/")[1].strip().rstrip("%"))
        except (OSError, ValueError, IndexError, subprocess.SubprocessError):
            now = 60
        new = max(0, min(100, now + step))
        try:
            self.pactl("set-sink-volume", sink, f"{new}%")
        except (OSError, subprocess.SubprocessError):
            return now
        print(f"volume {new} %", flush=True)
        self.publish("volume", str(new))
        subprocess.Popen(self.as_pulse_user + ["gst-launch-1.0", "-q", "audiotestsrc", "freq=1000", "num-buffers=3",
                                               "samplesperbuffer=960", "volume=0.3", "!", "audioconvert", "!", "pulsesink"],
                         env=self.pulse_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return new

    def audio_playing(self):
        try:
            return bool(self.pactl("list", "short", "sink-inputs").strip())
        except (OSError, subprocess.SubprocessError):
            return False

    # ---------- MQTT ----------
    def publish(self, topic, value, retain=True):
        if topic in ("orientation", "tilt", "motion", "light"):
            self.sensor_state[topic] = value                  # re-sent on (re)connect
        if self.mq:
            self.mq.publish(f"{BASE}/{topic}", value if isinstance(value, str) else json.dumps(value), retain=retain)

    def report_screen(self):
        self.publish("screen", {"state": "OFF" if self.standby else "ON", "brightness": self.brightness})

    def discovery(self):
        dev = {"identifiers": [ID], "name": NAME, "model": MODEL, "manufacturer": "Lenovo", "sw_version": "tablethw 2"}
        avail = {"availability_topic": f"{BASE}/status"}

        def cfg(comp, obj, extra):
            c = {"unique_id": f"{ID}_{obj}", "object_id": f"{ID}_{obj}", "device": dev, **avail, **extra}
            self.mq.publish(f"homeassistant/{comp}/{ID}/{obj}/config", json.dumps(c), retain=True)

        cfg("light", "screen", {"name": "Screen", "schema": "json", "state_topic": f"{BASE}/screen",
            "command_topic": f"{BASE}/screen/set", "brightness": True, "brightness_scale": 255, "icon": "mdi:tablet"})
        for colour in (("red", "green") if HAS_LEDS else ()):
            cfg("light", f"led_{colour}", {"name": f"LED {colour}", "schema": "json", "state_topic": f"{BASE}/led_{colour}",
                "command_topic": f"{BASE}/led_{colour}/set", "effect": True, "effect_list": ["solid", "blink", "fast"], "icon": "mdi:led-on"})
        cfg("button", "vibrate", {"name": "Vibrate", "command_topic": f"{BASE}/vibrate/press", "icon": "mdi:vibrate"})
        cfg("event", "button", {"name": "Button", "state_topic": f"{BASE}/button",
            "event_types": ["power", "power_long", "volume_up", "volume_down"], "icon": "mdi:gesture-tap-button"})
        cfg("sensor", "volume", {"name": "Volume", "state_topic": f"{BASE}/volume", "unit_of_measurement": "%", "icon": "mdi:volume-high"})
        cfg("sensor", "battery", {"name": "Battery", "state_topic": f"{BASE}/battery", "value_template": "{{ value_json.percent }}",
            "unit_of_measurement": "%", "device_class": "battery", "state_class": "measurement"})
        cfg("sensor", "battery_power", {"name": "Battery charge power", "state_topic": f"{BASE}/battery", "value_template": "{{ value_json.watt }}",
            "unit_of_measurement": "W", "device_class": "power", "state_class": "measurement"})
        cfg("sensor", "battery_temp", {"name": "Battery temperature", "state_topic": f"{BASE}/battery", "value_template": "{{ value_json.temp }}",
            "unit_of_measurement": "°C", "device_class": "temperature", "state_class": "measurement", "entity_category": "diagnostic"})
        cfg("sensor", "cpu_temp", {"name": "CPU temperature", "state_topic": f"{BASE}/battery", "value_template": "{{ value_json.cpu }}",
            "unit_of_measurement": "°C", "device_class": "temperature", "state_class": "measurement", "entity_category": "diagnostic"})
        cfg("binary_sensor", "charging", {"name": "Charging", "state_topic": f"{BASE}/battery",
            "value_template": "{{ 'ON' if value_json.charging else 'OFF' }}", "device_class": "battery_charging"})
        if self.has_acc:
            cfg("sensor", "orientation", {"name": "Orientation", "state_topic": f"{BASE}/orientation", "device_class": "enum",
                "options": ["portrait", "portrait_upside_down", "landscape_left", "landscape_right", "flat_screen_up", "flat_screen_down"],
                "icon": "mdi:phone-rotate-landscape"})
            cfg("sensor", "tilt", {"name": "Tilt", "state_topic": f"{BASE}/tilt", "unit_of_measurement": "°",
                "state_class": "measurement", "icon": "mdi:angle-acute", "entity_category": "diagnostic"})
            cfg("binary_sensor", "motion", {"name": "Motion", "state_topic": f"{BASE}/motion", "device_class": "moving"})
        if self.acc_source == "pipe":
            cfg("sensor", "light", {"name": "Illuminance", "state_topic": f"{BASE}/light", "unit_of_measurement": "lx",
                "device_class": "illuminance", "state_class": "measurement"})

    def on_connect(self, c, u, f, rc, props=None):
        print("mqtt connected", rc, flush=True)
        c.publish(f"{BASE}/status", "online", retain=True)
        self.discovery()
        for t in ("screen/set", "vibrate/press") + (("led_red/set", "led_green/set") if HAS_LEDS else ()):
            c.subscribe(f"{BASE}/{t}")
        self.report_screen()
        for colour, s in (self.leds.items() if HAS_LEDS else ()):
            self.publish(f"led_{colour}", {"state": "ON" if s["on"] else "OFF", "effect": s["effect"] or "solid"})
        self.report_battery()
        for k, v in list(self.sensor_state.items()):
            self.publish(k, v)

    def on_message(self, c, u, m):
        # MQTT is a network input: validate it, and never let a bad message kill the MQTT thread
        try:
            t = m.topic[len(BASE) + 1:]
            try:
                p = json.loads(m.payload or b"{}")
            except ValueError:
                p = {"state": m.payload.decode(errors="ignore")[:16]}
            if not isinstance(p, dict):
                return                                  # not a JSON object: ignore
            state_on = str(p.get("state", "ON")).upper() == "ON"
            if t == "screen/set":
                b = p.get("brightness")
                b = int(b) if isinstance(b, (int, float)) and not isinstance(b, bool) and 0 <= b <= 255 else None
                self.set_screen(state_on, b, "ha")
            elif t in ("led_red/set", "led_green/set") and HAS_LEDS:
                colour = t.split("/")[0][4:]
                effect = p.get("effect", self.leds[colour]["effect"])
                if effect not in (None, "solid", "blink", "fast"):
                    effect = None
                self.set_led(colour, state_on, None if effect == "solid" else effect)
            elif t == "vibrate/press":
                self.vibrate(200)
        except Exception as e:
            print("mqtt message ignored:", repr(e), flush=True)

    def start_mqtt(self):
        m = CFG["mqtt"]
        c = mqtt.Client(client_id=f"tablethw-{ID}")
        c.username_pw_set(m["user"], m["pass"])
        c.will_set(f"{BASE}/status", "offline", retain=True)
        c.on_connect = self.on_connect
        c.on_message = self.on_message
        c.reconnect_delay_set(2, 60)
        c.connect_async(m["host"], int(m.get("port", 1883)), 60)
        c.loop_start()
        self.mq = c

    # ---------- battery and temperature ----------
    def report_battery(self):
        b = "/sys/class/power_supply/battery/"
        try:
            i = float(read(b + "current_now", 0)) / 1e6; u = float(read(b + "voltage_now", 0)) / 1e6
        except ValueError:
            i = u = 0
        cpu = None
        for z in os.listdir("/sys/class/thermal"):
            if read(f"/sys/class/thermal/{z}/type") == "mtktscpu":
                cpu = round(int(read(f"/sys/class/thermal/{z}/temp", 0)) / 1000, 1)
        # positive current_now = discharging on these kernels, so the charge power is -I·U
        self.publish("battery", {"percent": int(read(b + "capacity", 0)), "charging": read(b + "status") in ("Charging", "Full"),
                                 "watt": round(-i * u, 2), "temp": round(int(read(b + "temp", 0)) / 10, 1), "cpu": cpu})

    def battery_loop(self):
        while True:
            self.report_battery()
            time.sleep(60)

    # ---------- accelerometer and light ----------
    # M7: MediaTek hwmsensor ioctl GSENSOR_IOCTL_READ_SENSORDATA = _IOR(0x85, 0x03, int) fills a buffer with "x y z" as
    # hex text (signed 32-bit) in mm/s². Axes: z out of the screen, y along the long side (portrait = natural).
    ACC_REQ = (2 << 30) | (4 << 16) | (0x85 << 8) | 0x03

    def acc_read(self, fd):
        b = bytearray(64); fcntl.ioctl(fd, self.ACC_REQ, b, True)
        return [(lambda v: v - (1 << 32) if v & 0x80000000 else v)(int(w, 16)) for w in bytes(b).split(b"\0")[0].split()][:3]

    @staticmethod
    def orientation(x, y, z):
        ax, ay, az = abs(x), abs(y), abs(z)
        if az > 0.8 * 9807: return "flat_screen_up" if z > 0 else "flat_screen_down"
        if ay >= ax: return "portrait" if y > 0 else "portrait_upside_down"
        return "landscape_left" if x > 0 else "landscape_right"

    def acc_samples(self):
        # yields (x, y, z) in mm/s², from /dev/gsensor (M7) or from the sensor pipe (M8: m/s² lines, plus light)
        if self.acc_source == "gsensor":
            fd = os.open("/dev/gsensor", os.O_RDONLY)
            failed = False
            while True:
                try:
                    yield self.acc_read(fd)
                except (OSError, ValueError) as e:
                    if not failed: print("acc:", repr(e), flush=True); failed = True
                    time.sleep(1); continue
                time.sleep(0.05)
        lux_sent, lux_time = None, 0
        while True:
            try:
                with open(SENSOR_PIPE) as f:                   # blocks until the Android side writes
                    for line in f:
                        try:
                            m = json.loads(line)
                        except ValueError:
                            continue
                        if m.get("sensor") == "accelerometer":
                            yield [int(m[k] * 1000) for k in ("x", "y", "z")]
                        elif m.get("sensor") == "light":
                            lux, now = float(m["value"]), time.time()
                            if lux_sent is None or abs(lux - lux_sent) > max(2, 0.15 * lux_sent) or now - lux_time > 300:
                                lux_sent, lux_time = lux, now; self.publish("light", f"{lux:.0f}")
            except OSError as e:
                print("sensor pipe:", repr(e), flush=True)
            time.sleep(3)

    def acc_loop(self):
        avg = None; current = None; candidate = None; since = 0; tilt_sent = None
        motion_until = 0; moving = False; last = 0
        for x, y, z in self.acc_samples():
            if avg is None:
                avg = [x, y, z]; self.publish("motion", "OFF")
            # knock or pick-up: deviation from the running average (low-pass, ~1 s) above 1.2 m/s²
            dev = math.sqrt(sum((a - g) ** 2 for a, g in zip((x, y, z), avg)))
            avg = [g + (a - g) * 0.05 for a, g in zip((x, y, z), avg)]
            now = time.time()
            if dev > 1200: motion_until = now + 5
            if (now < motion_until) != moving:
                moving = now < motion_until
                self.publish("motion", "ON" if moving else "OFF")
            gx, gy, gz = avg
            o = self.orientation(gx, gy, gz)
            tilt = round(math.degrees(math.acos(max(-1, min(1, gz / max(1, math.sqrt(gx * gx + gy * gy + gz * gz)))))))
            # a new orientation counts once it is stable for 1.5 s (held in a hand, "motion" is on all the time)
            if o != candidate: candidate, since = o, now
            if o != current and now - since >= 1.5:
                current = o; self.publish("orientation", o)
            if tilt_sent is None or (abs(tilt - tilt_sent) >= 3 and now - last > 2) or now - last > 300:
                tilt_sent = tilt; last = now; self.publish("tilt", str(tilt))

    # ---------- automatic standby ----------
    def standby_loop(self):
        after = int(CFG.get("standby_after", 300))
        if after <= 0:
            return
        while True:
            time.sleep(15)
            if self.standby or time.time() - self.last_activity < after:
                continue
            if self.audio_playing():
                self.last_activity = time.time(); continue
            self.set_screen(False, None, f"{after} s without touch")

    # ---------- buttons and touch ----------
    def button_event(self, kind):
        print(time.strftime("%H:%M:%S"), "button:", kind, flush=True)
        self.publish("button", {"event_type": kind}, retain=False)

    def button_loop(self, name):
        dev = next(evdev.InputDevice(p) for p in evdev.list_devices() if evdev.InputDevice(p).name == name)
        down = {}
        for e in dev.read_loop():
            if e.type != evdev.ecodes.EV_KEY:
                continue
            self.last_activity = time.time()
            if e.value == 1:
                down[e.code] = time.time()
                if e.code in (evdev.ecodes.KEY_VOLUMEUP, evdev.ecodes.KEY_VOLUMEDOWN):
                    up = e.code == evdev.ecodes.KEY_VOLUMEUP
                    self.button_event("volume_up" if up else "volume_down")
                    if self.standby:
                        self.set_screen(True, None, "volume button")          # only wake up
                    else:
                        threading.Thread(target=self.change_volume, args=(6 if up else -6,), daemon=True).start()
            elif e.value == 0 and e.code == evdev.ecodes.KEY_POWER:
                # the M8 reports one press twice (~1 s apart): ignore a second one within 1.5 s
                with self.lock:
                    if time.time() - self.last_power < 1.5:
                        continue
                    self.last_power = time.time()
                if time.time() - down.get(e.code, time.time()) > 1.5:
                    self.button_event("power_long")
                else:
                    self.button_event("power")
                    self.set_screen(self.standby, None, "power button")

    def touch_loop(self, name):
        # mtk-tpd reports BTN_TOUCH; fts_ts (M8) and Himax (M11) only ABS_MT_TRACKING_ID (>= 0 = finger down, -1 = lifted)
        self.touch = next(evdev.InputDevice(p) for p in evdev.list_devices() if evdev.InputDevice(p).name == name)
        if self.standby:
            self.grab_touch(True)
        fingers = 0
        has_btn = evdev.ecodes.BTN_TOUCH in self.touch.capabilities().get(evdev.ecodes.EV_KEY, [])
        for e in self.touch.read_loop():
            self.last_activity = time.time()
            if e.type == evdev.ecodes.EV_KEY and e.code == evdev.ecodes.BTN_TOUCH:
                pressed, released = e.value == 1, e.value == 0
            elif e.type == evdev.ecodes.EV_ABS and e.code == evdev.ecodes.ABS_MT_TRACKING_ID and not has_btn:
                fingers = max(0, fingers + (1 if e.value >= 0 else -1))
                pressed, released = e.value >= 0, e.value < 0 and fingers == 0
            else:
                continue
            if pressed and self.standby:
                # wake up; the touch stays grabbed until the finger lifts, then goes back to the app
                self.wake_tap = True
                with self.lock:
                    self.standby = False
                    self.save()
                    self.dpms(True)
                    write(LEDS + "lcd-backlight/brightness", self.brightness)
                self.report_screen()
                print(time.strftime("%H:%M:%S"), "screen on (tap)", flush=True)
            elif released and self.wake_tap:
                self.wake_tap = False
                with self.lock:
                    self.grab_touch(False)

    # ---------- local HTTP for the app ----------
    def http(self):
        hw = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, d):
                b = json.dumps(d).encode()
                self.send_response(200); self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

            def do_GET(self):
                if urlparse(self.path).path == "/status":
                    self.reply({"standby": hw.standby, "brightness": hw.brightness})
                else:
                    self.send_error(404)

            def do_POST(self):
                u = urlparse(self.path); q = parse_qs(u.query)
                if u.path == "/wake":
                    if hw.standby:
                        hw.set_screen(True, None, "app")
                    self.reply({"ok": True})
                elif u.path == "/vibrate":
                    hw.vibrate(q.get("ms", ["40"])[0]); self.reply({"ok": True})
                else:
                    self.send_error(404)

        ThreadingHTTPServer(("127.0.0.1", 8091), H).serve_forever()


def main():
    hw = Hw()
    write(LEDS + "lcd-backlight/brightness", 0 if hw.standby else hw.brightness)
    names = {evdev.InputDevice(p).name for p in evdev.list_devices()}
    if os.path.exists(SENSOR_PIPE):
        # The Android side delivers accelerometer and light through the pipe. (Once it enables the accelerometer,
        # /dev/gsensor returns values on the M8 too, but then the light sensor is missing: the pipe wins.)
        hw.has_acc, hw.acc_source = True, "pipe"
    else:
        try:
            fd = os.open("/dev/gsensor", os.O_RDONLY)
            for _ in range(5):
                if any(hw.acc_read(fd)): hw.has_acc, hw.acc_source = True, "gsensor"; break
                time.sleep(0.1)
            os.close(fd)
        except (OSError, ValueError):
            pass
    print("buttons:", [n for n in BUTTON_DEVICES if n in names], "· touch:", [n for n in TOUCH_DEVICES if n in names][:1],
          "· LEDs:", HAS_LEDS, "· motion:", hw.acc_source or "none", flush=True)
    hw.start_mqtt()
    tasks = [(hw.button_loop, (n,)) for n in BUTTON_DEVICES if n in names]
    tasks += [(hw.touch_loop, (n,)) for n in TOUCH_DEVICES if n in names][:1]
    tasks += [(hw.battery_loop, ()), (hw.standby_loop, ())] + ([(hw.acc_loop, ())] if hw.has_acc else [])
    for target, args in tasks:
        threading.Thread(target=target, args=args, daemon=True).start()
    print("tablethw started", flush=True)
    hw.http()


if __name__ == "__main__":
    main()
