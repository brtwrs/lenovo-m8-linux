// Start a binder thread pool in a standalone (non-app) binary: without one, services can't call back into us
// (e.g. the camera service blocks dequeueing buffers from our surface). Android <29 has no NDK call for this, so use
// libbinder's ProcessState::self()->startThreadPool() through dlsym.
// Written in C++ on purpose: ProcessState::self() returns sp<ProcessState>, a class with a non-trivial destructor,
// which the ABI returns through a hidden pointer (r0 on arm32, x8 on arm64). Declaring the same shape here lets
// the compiler get that right on both.
#include <dlfcn.h>
#include <stdio.h>

namespace {
struct SpProcessState {            // same layout as android::sp<android::ProcessState>: one pointer
    void *ptr = nullptr;
    ~SpProcessState() {}           // non-trivial: forces the indirect return convention, like the real sp<>
};
}

extern "C" void start_binder_threads(void) {
    void *lib = dlopen("libbinder.so", RTLD_NOW);
    if (!lib) { fprintf(stderr, "libbinder not usable\n"); return; }
    auto self = reinterpret_cast<SpProcessState (*)()>(dlsym(lib, "_ZN7android12ProcessState4selfEv"));
    auto start = reinterpret_cast<void (*)(void *)>(dlsym(lib, "_ZN7android12ProcessState15startThreadPoolEv"));
    if (!self || !start) { fprintf(stderr, "libbinder symbols missing\n"); return; }
    SpProcessState ps = self();    // leaks one strong reference on purpose: the pool lives as long as the process
    if (ps.ptr) start(ps.ptr);
}
