/* fakeperm: stand-ins for the system_server services that native Android services wait for in Linux mode.
 *
 * Without system_server nobody answers these binder services, and audioserver / cameraserver block on them
 * forever: audioserver waits for "activity" while starting its audio policy, and every permission check waits
 * for "permission". Opening an audio stream or setting a volume then just hangs.
 *
 * Registered here:
 *   permission       android.os.IPermissionController   checkPermission -> granted, noteOp -> MODE_ALLOWED
 *   activity         android.app.IActivityManager        every call succeeds with zero/false
 *   appops           com.android.internal.app.IAppOpsService  every call -> 0 (MODE_ALLOWED)
 *   sensor_privacy   android.hardware.ISensorPrivacyManager   every call -> 0 (privacy off: mic and camera allowed)
 *   permission_checker  android.permission.IPermissionChecker  checkPermission -> 0 (PERMISSION_GRANTED); Android 12+
 *   scheduling_policy   android.os.ISchedulingPolicyService    requestPriority -> 0. Without it audioserver's FastMixer
 *                       retries every second before each HAL write, so nothing ever reaches the speaker.
 *   package_native      android.content.pm.IPackageManagerNative  every call -> 0 (empty list, empty string, false).
 *                       MediaCodec::configure waits for it on Android 12 (format shaper), so no encoder without it.
 *   processinfo         android.os.IProcessInfoService  every pid: PROCESS_STATE_TOP, oom score 0. CameraService
 *                       asks this before every connect; without an answer of the right length it refuses.
 * A service that already exists (e.g. on a normal Android boot) is skipped.
 *
 * Only run this in Linux mode: the Android UI and its apps are stopped there, and the only clients are your own
 * root processes. It grants everything.
 *
 * Uses libbinder_ndk only (stable across Android versions). The service-manager and thread-pool calls are
 * not in the NDK's link stubs, so they are looked up at runtime; libbinder_ndk exports them since Android 10.
 */
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* IPermissionController.aidl */
enum { CHECK_PERMISSION = FIRST_CALL_TRANSACTION, NOTE_OP };

static void *on_create(void *args) { return args; }
static void on_destroy(void *data) {}

static binder_status_t reply_ok(AParcel *out) {
    AStatus *ok = AStatus_newOk();
    binder_status_t s = AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    return s;
}

static binder_status_t permission_transact(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out) {
    reply_ok(out);
    if (code == CHECK_PERMISSION) return AParcel_writeBool(out, true);   /* granted */
    return AParcel_writeInt32(out, 0);                                    /* noteOp: MODE_ALLOWED; others: 0 */
}

/* everything else: success, followed by a zero (false, MODE_ALLOWED, null parcelable, empty array) */
static binder_status_t neutral_transact(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out) {
    reply_ok(out);
    return AParcel_writeInt32(out, 0);
}

/* IProcessInfoService (frameworks/native/libs/binder/IProcessInfoService.cpp): the client writes int[] pids plus the
   lengths of the out arrays; the reply is the states array (and, for code 2, the scores array), each as length + values */
enum { GET_PROCESS_STATES_FROM_PIDS = FIRST_CALL_TRANSACTION, GET_PROCESS_STATES_AND_OOM_SCORES_FROM_PIDS };
#define PROCESS_STATE_TOP 2
static binder_status_t processinfo_transact(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out) {
    int32_t n = 0;
    if (AParcel_readInt32(in, &n) != STATUS_OK || n < 0 || n > 4096) return STATUS_BAD_VALUE;
    reply_ok(out);
    AParcel_writeInt32(out, n);
    for (int32_t i = 0; i < n; i++) AParcel_writeInt32(out, PROCESS_STATE_TOP);
    if (code == GET_PROCESS_STATES_AND_OOM_SCORES_FROM_PIDS) {
        AParcel_writeInt32(out, n);
        for (int32_t i = 0; i < n; i++) AParcel_writeInt32(out, 0);
    }
    return STATUS_OK;
}

static const struct { const char *name, *descriptor; AIBinder_Class_onTransact fn; } SERVICES[] = {
    { "permission",     "android.os.IPermissionController",        permission_transact },
    { "activity",       "android.app.IActivityManager",            neutral_transact },
    { "appops",         "com.android.internal.app.IAppOpsService", neutral_transact },
    { "sensor_privacy", "android.hardware.ISensorPrivacyManager",  neutral_transact },
    { "permission_checker", "android.permission.IPermissionChecker", neutral_transact },
    { "scheduling_policy",  "android.os.ISchedulingPolicyService",   neutral_transact },
    { "package_native",     "android.content.pm.IPackageManagerNative", neutral_transact },
    { "processinfo",        "android.os.IProcessInfoService",        processinfo_transact },
};

int main(void) {
    void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
    binder_exception_t (*add_service)(AIBinder *, const char *) = ndk ? dlsym(ndk, "AServiceManager_addService") : NULL;
    AIBinder *(*check_service)(const char *) = ndk ? dlsym(ndk, "AServiceManager_checkService") : NULL;
    bool (*set_max)(uint32_t) = ndk ? dlsym(ndk, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
    void (*start_pool)(void) = ndk ? dlsym(ndk, "ABinderProcess_startThreadPool") : NULL;
    void (*join_pool)(void) = ndk ? dlsym(ndk, "ABinderProcess_joinThreadPool") : NULL;
    if (!add_service || !check_service || !set_max || !start_pool || !join_pool) {
        fprintf(stderr, "libbinder_ndk lacks the service-manager calls (Android < 10?)\n"); return 1;
    }
    int added = 0;
    for (size_t i = 0; i < sizeof SERVICES / sizeof SERVICES[0]; i++) {
        AIBinder *existing = check_service(SERVICES[i].name);
        if (existing) { AIBinder_decStrong(existing); fprintf(stderr, "fakeperm: %s exists, skipped\n", SERVICES[i].name); continue; }
        AIBinder_Class *cls = AIBinder_Class_define(SERVICES[i].descriptor, on_create, on_destroy, SERVICES[i].fn);
        binder_exception_t e = add_service(AIBinder_new(cls, NULL), SERVICES[i].name);
        if (e != EX_NONE) fprintf(stderr, "fakeperm: addService(%s) failed: %d\n", SERVICES[i].name, e);
        else { fprintf(stderr, "fakeperm: %s registered\n", SERVICES[i].name); added++; }
    }
    if (!added) return 0;
    set_max(2);
    start_pool();
    join_pool();
    return 0;
}
