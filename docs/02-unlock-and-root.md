# 2. Unlock the bootloader and root

> ⚠️ Unlocking **wipes the tablet**. Do the backup first.

## 2.1 BROM backup (do this first)

With mtkclient, put the tablet in BROM mode (power off, hold **Vol+ and Vol−** while plugging in USB) and dump every
partition except `userdata`, `super` and `metadata` (those are huge and not needed to recover a bricked boot chain):

```sh
python3 mtk.py rl backup/ --skip=userdata,super,metadata
```

Tip: mtkclient must catch the BROM window within a second or two. If it keeps missing it (USB passthrough into a
VM/container adds delay), run it on a machine with a direct USB port. On our setup a Mac worked where a Proxmox
container did not.

## 2.2 Unlock

Enable Developer options → **OEM unlocking** and USB debugging, then:

```sh
adb reboot bootloader
fastboot flashing unlock        # confirm on the tablet with Volume UP
```

The USB session may hang afterwards; hold Power to restart. The tablet is wiped and now boots with an orange state
warning. Enable USB debugging again.

## 2.3 Root with Magisk (patch boot_a on the tablet)

Install Magisk, copy `boot_a` from your backup to the tablet and patch it with Magisk's `boot_patch.sh` (the arm64
tools from the Magisk APK), then flash it:

```sh
fastboot flash boot_a magisk_patched.img
```

Magisk asks for "additional setup" on first start: accept and reboot. Grant root to the shell
(Magisk → Superuser) and check `adb shell su -c id`.
