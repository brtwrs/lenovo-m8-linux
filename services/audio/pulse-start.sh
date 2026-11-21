#!/bin/sh
# Start PulseAudio (see m7.pa). Clients find it through /etc/pulse/client.conf.d/m7.conf:
#   default-server = unix:/run/m7pulse/native
#   autospawn = no
# and ALSA programs through /etc/asound.conf:  pcm.!default { type pulse }  ctl.!default { type pulse }
mkdir -p /run/m7pulse && chmod 700 /run/m7pulse
# PulseAudio runs as root: once m7.pa is loaded, clients may not load modules (a module could write files as root)
# or stop the daemon. Keep this if you ever give non-root users access to the client socket.
exec pulseaudio -n -F /root/m7-audio/m7.pa --daemonize=no --exit-idle-time=-1 --log-target=stderr --disallow-module-loading --disallow-exit
