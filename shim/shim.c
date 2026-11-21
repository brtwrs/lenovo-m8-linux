/* Android 11 symbols that Halium 11's libhwc2_compat_layer needs but Android 12 no longer exports.
 * Built without glibc (-nostdlib); operator delete comes from Android's libc++. Add it to the compat layer with
 *   patchelf --add-needed libm8shim.so libhwc2_compat_layer.so */
extern void _ZdlPv(void *);                     /* operator delete(void*) */
/* android::HdrCapabilities::~HdrCapabilities(): the first member is a std::vector<ui::Hdr> */
struct vec { void *begin, *end, *cap; };
void _ZN7android15HdrCapabilitiesD1Ev(struct vec *self) { if (self->begin) _ZdlPv(self->begin); }
void _ZN7android15HdrCapabilitiesD2Ev(struct vec *self) { _ZN7android15HdrCapabilitiesD1Ev(self); }
