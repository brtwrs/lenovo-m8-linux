# 5. GPU: libhybris

The PowerVR driver is an Android (bionic) blob. libhybris loads it into glibc programs with a bionic linker taken
from a Halium system image. The M8 needs a **mix of two Halium versions**:

| Piece | From | Why |
|---|---|---|
| bionic `libc.so`, `libm.so`, `libdl.so`, `libdl_android.so` | LuneOS **Halium 13** arm64 system image | matches Android 12's ABI |
| `libhwc2_compat_layer.so`, `libui_compat_layer.so`, `android.frameworks.vr.composer@1.0.so` | LuneOS **Halium 11** arm64 | Halium 13's compat layer needs symbols Android 12 doesn't have |
| `libm8shim.so` (`shim/shim.c`) | this repo | the Halium 11 compat layer needs one Android 11 symbol that Android 12 dropped |
| i18n libs (ICU) | Android's runtime apex | copy them to `/opt/hybris/android/i18n`, last in the search path |

Put the Halium libraries in `/opt/hybris/android/lib64`. Build the shim without glibc and add it to the compat layer:

```sh
aarch64-linux-gnu-gcc -shared -fPIC -nostdlib -o libm8shim.so shim/shim.c
patchelf --add-needed libm8shim.so /opt/hybris/android/lib64/libhwc2_compat_layer.so
```

**libhybris** itself: build it on the tablet (arm64) against Halium's android-headers, branch `halium-11.0`
(`https://github.com/Halium/android-headers`):

```sh
cd libhybris/hybris && ./autogen.sh
./configure --prefix=/opt/hybris --enable-arch=arm64 --enable-wayland \
            --with-android-headers=/root/src/android-headers --with-default-egl-platform=wayland \
            --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64
make -j4 && make install
```

Environment: see `scripts/hybris-env.sh`. libhybris also needs Android's binder inside the chroot: bind
`/dev/binderfs` (the boot module does that in GPU mode). Check with `EGL_PLATFORM=hwcomposer test_egl` or start
Sway (chapter 6).
