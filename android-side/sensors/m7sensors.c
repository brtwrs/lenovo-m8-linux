/* m7sensors: read the tablet's sensors (accelerometer, light, proximity) in Linux mode through the vendor's
 * legacy sensors HAL module (/vendor/lib64/hw/sensors.<soc>.so), without Android's sensorservice (which lives in
 * system_server and doesn't run in Linux mode).
 *
 *   m7sensors list              list the sensors the HAL offers
 *   m7sensors dump <seconds>    enable accelerometer, light and proximity, print events
 *   m7sensors stream            same, forever, one JSON line per event (for the Linux side to read)
 *
 * Android's sensors HAL service (vendor.sensors-hal-*) must be stopped first: it reads the same kernel devices.
 * The linker won't let a /data binary load from /vendor: copy the module and its vendor-only dependencies to a
 * private directory, then run with M7SENSORS_MODULE=<dir>/sensors.<soc>.so and LD_LIBRARY_PATH=<dir>.
 * The structures below follow hardware/libhardware/include/hardware/{hardware,sensors}.h.
 */
#include <dlfcn.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __LP64__
#define RESERVED_T uint64_t
#else
#define RESERVED_T uint32_t
#endif

struct hw_module_t;
struct hw_device_t;
typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id, struct hw_device_t **device);
} hw_module_methods_t;
typedef struct hw_module_t {
    uint32_t tag; uint16_t module_api_version; uint16_t hal_api_version;
    const char *id, *name, *author; hw_module_methods_t *methods; void *dso; RESERVED_T reserved[32 - 7];
} hw_module_t;
typedef struct hw_device_t {
    uint32_t tag; uint32_t version; struct hw_module_t *module; RESERVED_T reserved[12];
    int (*close)(struct hw_device_t *device);
} hw_device_t;

typedef struct sensor_t {
    const char *name, *vendor; int version, handle, type; float maxRange, resolution, power;
    int32_t minDelay; uint32_t fifoReservedEventCount, fifoMaxEventCount; const char *stringType, *requiredPermission;
#ifdef __LP64__
    int64_t maxDelay; uint64_t flags;
#else
    int32_t maxDelay; uint32_t flags;
#endif
    void *reserved[2];
} sensor_t;

typedef struct sensors_event_t {
    int32_t version, sensor, type, reserved0; int64_t timestamp;
    float data[16]; uint64_t u64[8]; uint32_t flags; uint32_t reserved1[3];
} sensors_event_t;

typedef struct sensors_module_t {
    hw_module_t common;
    int (*get_sensors_list)(struct sensors_module_t *module, const sensor_t **list);
    int (*set_operation_mode)(unsigned int mode);
} sensors_module_t;

typedef struct sensors_poll_device_1 {
    hw_device_t common;
    int (*activate)(struct sensors_poll_device_1 *dev, int handle, int enabled);
    int (*setDelay)(struct sensors_poll_device_1 *dev, int handle, int64_t period_ns);
    int (*poll)(struct sensors_poll_device_1 *dev, sensors_event_t *data, int count);
    int (*batch)(struct sensors_poll_device_1 *dev, int handle, int flags, int64_t period_ns, int64_t timeout);
    int (*flush)(struct sensors_poll_device_1 *dev, int handle);
    void *rest[8];
} sensors_poll_device_1_t;

enum { TYPE_ACCELEROMETER = 1, TYPE_LIGHT = 5, TYPE_PROXIMITY = 8 };

static const char *type_name(int t) {
    return t == TYPE_ACCELEROMETER ? "accelerometer" : t == TYPE_LIGHT ? "light" : t == TYPE_PROXIMITY ? "proximity" : NULL;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "list";
    glob_t g;
#ifdef __LP64__
    glob("/vendor/lib64/hw/sensors.*.so", 0, NULL, &g);
#else
    glob("/vendor/lib/hw/sensors.*.so", 0, NULL, &g);
#endif
    const char *path = getenv("M7SENSORS_MODULE");
    if (!path && g.gl_pathc) path = g.gl_pathv[0];
    if (!path) { fprintf(stderr, "no legacy sensors HAL module in /vendor\n"); return 1; }
    void *lib = dlopen(path, RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); return 1; }
    sensors_module_t *m = dlsym(lib, "HMI");
    if (!m) { fprintf(stderr, "no HAL_MODULE_INFO_SYM in %s\n", path); return 1; }
    fprintf(stderr, "module: %s (%s)\n", m->common.name, path);

    sensors_poll_device_1_t *dev = NULL;
    if (m->common.methods->open(&m->common, "poll", (hw_device_t **)&dev) || !dev) { fprintf(stderr, "open poll device failed\n"); return 1; }
    /* MediaTek's module only builds its sensor list when the poll device is opened: list after open */
    const sensor_t *list = NULL;
    int n = m->get_sensors_list(m, &list);
    if (!strcmp(mode, "list")) {
        for (int i = 0; i < n; i++)
            printf("handle %2d  type %3d  %-36s %s  max %.1f\n", list[i].handle, list[i].type, list[i].name, list[i].vendor, list[i].maxRange);
        return 0;
    }

    for (int i = 0; i < n; i++) {
        if (!type_name(list[i].type)) continue;
        if (dev->batch) dev->batch(dev, list[i].handle, 0, 100000000LL, 0);      /* 10 Hz */
        else dev->setDelay(dev, list[i].handle, 100000000LL);
        int r = dev->activate(dev, list[i].handle, 1);
        fprintf(stderr, "enabled %s (handle %d): %d\n", type_name(list[i].type), list[i].handle, r);
    }
    int seconds = (!strcmp(mode, "dump") && argc > 2) ? atoi(argv[2]) : -1;
    time_t end = time(NULL) + seconds;
    sensors_event_t ev[16];
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (seconds < 0 || time(NULL) < end) {
        int k = dev->poll(dev, ev, 16);
        for (int i = 0; i < k; i++) {
            const char *t = type_name(ev[i].type);
            if (!t) continue;
            if (ev[i].type == TYPE_ACCELEROMETER)
                printf("{\"sensor\": \"%s\", \"x\": %.3f, \"y\": %.3f, \"z\": %.3f}\n", t, ev[i].data[0], ev[i].data[1], ev[i].data[2]);
            else
                printf("{\"sensor\": \"%s\", \"value\": %.2f}\n", t, ev[i].data[0]);
        }
    }
    for (int i = 0; i < n; i++) if (type_name(list[i].type)) dev->activate(dev, list[i].handle, 0);
    return 0;
}
