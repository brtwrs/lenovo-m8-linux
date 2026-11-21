# 6. Compositor: wlroots + Sway 1.6.1

**wlroots:** Droidian's fork with the hwcomposer backend (`https://github.com/droidian/wlroots`; tested at commit
`599e4aa4`), plus `patches/wlroots-droidian.diff`:
* no partial damage to the hwcomposer and a `glFinish()` before each swap (the MTK hwcomposer gets no fence),
* buffer age always unknown, and `wlr_egl_set_damage_region` a no-op: the PowerVR blob returns `EGL_BAD_ACCESS`
  without a buffer-age query.

Install it to `/opt/wlr`.

**Sway 1.6.1** is the newest Sway that still builds against that wlroots (0.12 API), after
`patches/sway-1.6.1.diff` (API renames, `touch_frame` forwarding, running as root) and
`patches/sway-wlroots-compat.c` (two helper functions the old wlroots lacks; the diff already lists it in
`sway/meson.build` as `m7compat.c`):

```sh
git clone https://github.com/swaywm/sway && cd sway && git checkout 1.6.1
git apply ../patches/sway-1.6.1.diff && cp ../patches/sway-wlroots-compat.c sway/m7compat.c
meson setup build --prefix=/opt/wlr -Dwerror=false && ninja -C build install
```

Start it with `scripts/sway-start.sh` (the boot module does that). Sway's socket is `/tmp/xdg/wayland-1`.

## Touch

* The real touchscreen is `fts_ts`. There is no udev in the chroot, so `sway-start.sh` writes the udev data libinput
  needs (`ID_INPUT_TOUCHSCREEN`, `ID_SEAT=seat1`).
* Sway already rotates touch together with `output * transform 90`: use **no** extra calibration matrix
  (`calibration_matrix 1 0 0 0 1 0`). A rotation matrix on top makes every tap land 180° off.
* `map_to_output *` does not mean "all outputs" in this Sway: it looks for an output named `*` and disables touch.
  Name the output (`HWCOMPOSER-1`).
* Sway 1.6.1 did not forward `touch_frame`; Qt waits for it before it delivers a tap. The diff adds it.

## Qt

Debian's arm64 Qt 5 is built for desktop OpenGL. On this GLES-only GPU install the GLES variants:
`libqt5gui5-gles libqt5quick5-gles`, and for building `qtbase5-gles-dev`. **Never install `qtbase5-dev`**: it
replaces the GLES Qt, and no Qt app gets an EGL surface anymore ("Cannot find EGLConfig", EGL error 0x3005).

Run your app as a Sway client: `WAYLAND_DISPLAY=wayland-1 EGL_PLATFORM=wayland QT_QPA_PLATFORM=wayland-egl` plus the
libhybris environment, and make it fullscreen with a `for_window` rule (`scripts/sway-config`).

Screenshots: `XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-1 grim shot.png`.
