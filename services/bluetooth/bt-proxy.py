#!/usr/bin/env python3
"""m7-bt: the Lenovo Tab M7 as an ESPHome Bluetooth proxy (passive, raw advertisements) for Home Assistant.

- BLE: raw HCI over MediaTek's /dev/stpbt (no BlueZ on this kernel). Passive scan at ~30 % duty,
  because bluetooth shares the 2.4 GHz antenna with wifi.
- ESPHome native API (plaintext) on :6053, only what a proxy needs; announced over mDNS.
- Config bt.json next to this file:
    {"name": "m7-bt", "friendly": "Tablet M7 BT", "allowed": ["<Home Assistant IP>"]}
  The plaintext ESPHome API has no authentication, so only the addresses in "allowed" may connect.
  Without an allowlist the service refuses every connection.
"""
import json, os, select, socket, struct, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
try:
    CFG = json.load(open(os.path.join(HERE, "bt.json")))
except (OSError, ValueError):
    CFG = {}
NAME = CFG.get("name", "m7-bt")
FRIENDLY = CFG.get("friendly", "Tablet M7 BT")
PORT = 6053
VERSION = "2025.10.0"
FLAGS = 1 | 32            # FEATURE_PASSIVE_SCAN | FEATURE_RAW_ADVERTISEMENTS
ALLOWED = set(CFG.get("allowed", []))
MAX_FRAME = 64 * 1024
MAX_CONNECTIONS = 4
MAX_PER_IP = 2
IDLE_MAX = 90              # s without a message between handshake and subscription


# ---------- protobuf (only what we need) ----------
def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        if n: out.append(b | 0x80)
        else: out.append(b); return bytes(out)

def field_int(nr, v): return varint(nr << 3) + varint(v) if v else b""
def field_sint(nr, v): return varint(nr << 3) + varint((v << 1) ^ (v >> 63))
def field_bytes(nr, b):
    if isinstance(b, str): b = b.encode()
    return varint((nr << 3) | 2) + varint(len(b)) + b if b else b""

def read_varint(buf, i):
    n = s = 0
    while True:
        b = buf[i]; i += 1; n |= (b & 0x7F) << s; s += 7
        if not b & 0x80: return n, i

def fields(buf):
    i, out = 0, {}
    while i < len(buf):
        k, i = read_varint(buf, i); nr, t = k >> 3, k & 7
        if t == 0: v, i = read_varint(buf, i)
        elif t == 2: ln, i = read_varint(buf, i); v = buf[i:i + ln]; i += ln
        elif t == 5: v = buf[i:i + 4]; i += 4
        elif t == 1: v = buf[i:i + 8]; i += 8
        else: break
        out[nr] = v
    return out


# ---------- BLE over HCI ----------
class Scanner:
    def __init__(self):
        self.subscribers = set()           # connections that want advertisements
        self.lock = threading.RLock()
        self.mac = self.own_mac()

    @staticmethod
    def own_mac():
        # The chip reports MediaTek's default address (00:00:46:65:80:01), the same on every tablet.
        # HA uses it as the source ID, so derive a unique, locally administered address from the wifi MAC.
        try:
            w = bytes.fromhex(open("/sys/class/net/wlan0/address").read().strip().replace(":", ""))
            return bytes([w[0] | 0x02] + list(w[1:5]) + [(w[5] + 1) & 0xFF]).hex(":").upper()
        except (OSError, ValueError):
            return "02:00:46:65:80:01"

    def send(self, fd, ogf, ocf, data=b""):
        os.write(fd, bytes([0x01]) + struct.pack("<HB", (ogf << 10) | ocf, len(data)) + data)

    def loop(self):
        while True:
            try:
                self.scan()
            except Exception as e:             # never stop: log and start over
                print("bt:", repr(e), flush=True)
            time.sleep(5)

    def scan(self):
        fd = os.open("/dev/stpbt", os.O_RDWR | os.O_NOCTTY)
        try:
            buf = b""
            self.send(fd, 0x03, 0x0003); time.sleep(0.5)                                                       # reset
            self.send(fd, 0x08, 0x000B, bytes([0x00]) + struct.pack("<HH", 0x00A0, 0x0030) + bytes([0x00, 0x00]))  # passive, 100 ms / 30 ms
            self.send(fd, 0x08, 0x000C, bytes([0x01, 0x00]))                                                   # on, no duplicate filter
            print("bt: scanning", flush=True)
            batch, last = [], time.time()
            while True:
                r, _, _ = select.select([fd], [], [], 0.2)
                if r:
                    buf += os.read(fd, 4096)
                while len(buf) >= 3 and buf[0] == 0x04 and len(buf) >= 3 + buf[2]:
                    ev = buf[1:3 + buf[2]]; buf = buf[3 + buf[2]:]
                    if ev[0] == 0x3E and ev[2] == 0x02:                                                        # LE Advertising Report
                        n, p = ev[3], 4
                        for _ in range(n):
                            if p + 10 > len(ev) or p + 10 + ev[p + 8] > len(ev):
                                break                                                                          # truncated report: skip the rest
                            atype = ev[p + 1]; addr = int.from_bytes(ev[p + 2:p + 8], "little"); ln = ev[p + 8]
                            data = ev[p + 9:p + 9 + ln]; rssi = struct.unpack("b", ev[p + 9 + ln:p + 10 + ln])[0]
                            batch.append((addr, rssi, atype, data)); p += 10 + ln
                if buf and buf[0] != 0x04:
                    buf = b""
                if batch and (len(batch) >= 16 or time.time() - last > 0.1):
                    self.forward(batch); batch, last = [], time.time()
        finally:
            os.close(fd)

    def forward(self, batch):
        adv = b"".join(field_bytes(1, field_int(1, a) + field_sint(2, r) + field_int(3, t) + field_bytes(4, d)) for a, r, t, d in batch)
        with self.lock:
            subscribers = list(self.subscribers)   # copy: sending (and maybe stop()) happens outside the lock
        for c in subscribers:
            c.send(93, adv)


# ---------- ESPHome native API (plaintext) ----------
class Connection:
    def __init__(self, sock, scanner):
        self.sock, self.sc = sock, scanner
        # a slow client must not hold up the rest: only SENDING gets a timeout
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, struct.pack("ll", 2, 0))
        # handshake within 10 s, or close (idle connections must not hold a slot)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, struct.pack("ll", 10, 0))
        self.send_lock = threading.Lock()

    def send(self, kind, payload=b""):
        try:
            with self.send_lock:
                self.sock.sendall(b"\x00" + varint(len(payload)) + varint(kind) + payload)
        except OSError:
            self.stop()

    def stop(self):
        with self.sc.lock:
            self.sc.subscribers.discard(self)
        try: self.sock.shutdown(socket.SHUT_RDWR)  # makes a blocked recv() return, so the thread ends
        except OSError: pass
        try: self.sock.close()
        except OSError: pass

    def loop(self):
        buf = b""
        try:
            while True:
                d = self.sock.recv(4096)
                if not d: break
                buf += d
                if len(buf) > MAX_FRAME + 16:
                    return
                while len(buf) >= 3:
                    if buf[0] != 0:
                        print("api: not plaintext (encryption?)", flush=True); return
                    try:
                        ln, i = read_varint(buf, 1); kind, i = read_varint(buf, i)
                    except IndexError:
                        break
                    if ln > MAX_FRAME:
                        print("api: frame too large, closing", flush=True); return
                    if len(buf) < i + ln: break
                    self.message(kind, buf[i:i + ln]); buf = buf[i + ln:]
        except OSError:
            pass
        finally:
            self.stop()

    def message(self, kind, p):
        if kind == 1:       # Hello
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, struct.pack("ll", IDLE_MAX, 0))
            self.send(2, field_int(1, 1) + field_int(2, 10) + field_bytes(3, "m7-bt " + VERSION) + field_bytes(4, NAME))
        elif kind == 3:     # Authentication (no password)
            self.send(4)
        elif kind == 5:     # Disconnect
            self.send(6); self.stop()
        elif kind == 7:     # Ping
            self.send(8)
        elif kind == 9:     # DeviceInfo
            mac = self.sc.mac
            self.send(10, field_bytes(2, NAME) + field_bytes(3, mac) + field_bytes(4, VERSION) + field_bytes(5, time.strftime("%b %d %Y, %H:%M:%S"))
                      + field_bytes(6, "Lenovo Tab M7 (TB-7305F)") + field_int(15, FLAGS) + field_bytes(12, "Lenovo") + field_bytes(13, FRIENDLY)
                      + field_bytes(18, mac))
        elif kind == 11:    # ListEntities: none
            self.send(19)
        elif kind == 66:    # SubscribeBluetoothLEAdvertisements
            # HA only pings when it receives nothing; with a stream of advertisements it stays quiet, so no idle limit from here on.
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, struct.pack("ll", 0, 0))
            with self.sc.lock:
                self.sc.subscribers.add(self)
            print("api: subscribed to advertisements, flags", fields(p).get(1, 0), flush=True)
        elif kind == 87:    # Unsubscribe
            with self.sc.lock:
                self.sc.subscribers.discard(self)
        elif kind == 80:    # ConnectionsFree: no active connections supported
            self.send(81, field_int(1, 0) + field_int(2, 0))
        # 20, 28, 34, 38, 36 (states, logs, services, HA states, time): no reply needed


def mdns(scanner):
    try:
        from zeroconf import ServiceInfo, Zeroconf
    except ImportError:
        print("mdns: no zeroconf module", flush=True); return
    ip = None
    while not ip:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(("1.1.1.1", 80)); ip = s.getsockname()[0]; s.close()
        except OSError:
            time.sleep(5)
    time.sleep(3)
    info = ServiceInfo("_esphomelib._tcp.local.", f"{NAME}._esphomelib._tcp.local.", addresses=[socket.inet_aton(ip)], port=PORT,
                       properties={"version": VERSION, "mac": scanner.mac.replace(":", "").lower(), "platform": "linux", "board": "lenovo_tb_7305f",
                                   "network": "wifi", "friendly_name": FRIENDLY}, server=f"{NAME}.local.")
    Zeroconf().register_service(info)
    print("mdns: announced on", ip, flush=True)


def main():
    sc = Scanner()
    threading.Thread(target=sc.loop, daemon=True).start()
    threading.Thread(target=mdns, args=(sc,), daemon=True).start()
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT)); srv.listen(4)
    print("m7-bt started on", PORT, flush=True)
    if not ALLOWED:
        print("error: put the Home Assistant IP in bt.json \"allowed\"; without it m7-bt refuses all connections", flush=True)
    active = []                                    # [(thread, connection, ip)]
    while True:
        c, a = srv.accept()
        active = [x for x in active if x[0].is_alive()]
        if a[0] not in ALLOWED or len(active) >= MAX_CONNECTIONS or sum(1 for x in active if x[2] == a[0]) >= MAX_PER_IP:
            print("api: refused", a[0], flush=True); c.close(); continue
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("api: connection from", a[0], flush=True)
        conn = Connection(c, sc); t = threading.Thread(target=conn.loop, daemon=True); t.start(); active.append((t, conn, a[0]))


if __name__ == "__main__":
    main()
