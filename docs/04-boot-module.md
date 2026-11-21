# 4. Boot module

`magisk-module/m8linux` is a Magisk module. Copy it to `/data/adb/modules/m8linux/` (or zip and install it in the
Magisk app). It is the M7 project's module with the M8's GPU and media steps.

* `post-fs-data.sh` decides, very early, whether this boot becomes Linux: always, if
  `/data/adb/m8linux/linux-vast` exists (`mkdir -p /data/adb/m8linux; touch /data/adb/m8linux/linux-vast`).
* `linux-boot.sh` keeps stopping the Android UI (zygote, surfaceflinger), brings up wifi from Debian, starts sshd on
  2222, and in GPU mode binds `/dev/binderfs` into the chroot (libhybris needs it, otherwise
  "defaultServiceManager() is null") and starts Sway.
* **Safety net:** no ssh within 5 minutes → reboot, and the next boot is Android once.
* **Escape hatch:** Volume UP during the first 20 s → one Android boot. Not Volume DOWN: that is Magisk's safe mode.
* **Magisk's boot-loop counter:** Magisk counts every boot as failed until Android reports boot-complete, which never
  happens in Linux mode. After two Linux boots the third would start in safe mode, which disables the module.
  `linux-boot.sh` resets the counter once ssh is up.
* Log: `/data/adb/m8linux/m8linux.log`.
