#!/usr/bin/env python3
"""RTSP server for the M7 camera: rtsp://<tablet>:8554/camera, basic auth from cam.json next to this file:
    {"user": "m7", "password": "<random>", "port": 8554}      (chmod 600)

Reads raw H.264 from m7cam on the Android side (127.0.0.1:8093). Shared media: one camera session for all
viewers; when the last viewer leaves, the pipeline closes the TCP connection and m7cam turns the camera off."""
import json, os
import gi
gi.require_version("Gst", "1.0"); gi.require_version("GstRtspServer", "1.0")
from gi.repository import Gst, GstRtspServer, GLib

Gst.init(None)
cfg = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "cam.json")))
srv = GstRtspServer.RTSPServer(); srv.set_service(str(cfg.get("port", 8554)))
auth = GstRtspServer.RTSPAuth()
token = GstRtspServer.RTSPToken.new()
token.set_string(GstRtspServer.RTSP_TOKEN_MEDIA_FACTORY_ROLE, "viewer")
auth.add_basic(GstRtspServer.RTSPAuth.make_basic(cfg["user"], cfg["password"]), token)
srv.set_auth(auth)
f = GstRtspServer.RTSPMediaFactory()
f.set_launch("( tcpclientsrc host=127.0.0.1 port=8093 do-timestamp=true ! h264parse config-interval=-1 "
             "! rtph264pay name=pay0 pt=96 config-interval=1 )")
f.set_shared(True)
f.set_suspend_mode(GstRtspServer.RTSPSuspendMode.NONE)
perms = GstRtspServer.RTSPPermissions()
perms.add_permission_for_role("viewer", "media.factory.access", True)
perms.add_permission_for_role("viewer", "media.factory.construct", True)
f.set_permissions(perms)
srv.get_mount_points().add_factory("/camera", f)
srv.attach(None)
print("rtsp ready on", cfg.get("port", 8554), flush=True)
GLib.MainLoop().run()
