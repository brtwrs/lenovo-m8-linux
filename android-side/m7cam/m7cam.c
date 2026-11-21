/* m7cam: the Lenovo Tab M7's camera as raw H.264, for Linux mode.
 *
 * Runs on the Android side (bionic) next to minimediaservice, which provides the camera service.
 * Camera2 NDK -> MediaTek hardware H.264 encoder (input surface) -> Annex-B byte stream over TCP on
 * 127.0.0.1:<port>. On the Linux side, m7cam-rtsp.py turns that into an RTSP stream.
 *
 *   m7cam <camera-id> <port> <width> <height>        e.g. m7cam 1 8093 1280 720   (0 = back, 1 = front)
 *   M7CAM_BPS=2500000                                  bitrate
 *
 * - The camera only runs while a client is connected (and 30 s after the last one leaves).
 * - The encoder is created once at startup: on this MTK that takes ~9 s; the camera itself starts in ~0.5 s.
 * - Listens on 127.0.0.1 only, and only accepts clients running as root.
 */
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 8
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int client_fd[MAX_CLIENTS], client_ready[MAX_CLIENTS];
static unsigned char *sps_pps; static size_t sps_pps_len;
static AMediaCodec *codec;
static volatile int camera_on;

static void dev_disconnected(void *c, ACameraDevice *d) { fprintf(stderr, "camera disconnected\n"); exit(3); }
static void dev_error(void *c, ACameraDevice *d, int e) { fprintf(stderr, "camera error %d\n", e); exit(4); }
static void session_cb(void *c, ACameraCaptureSession *s) {}   /* must not be NULL: the session destructor calls onClosed */

static int send_all(int fd, const void *p, size_t n) {
    const char *q = p;
    while (n) { ssize_t k = send(fd, q, n, MSG_NOSIGNAL); if (k <= 0) return -1; q += k; n -= k; }
    return 0;
}

static void request_keyframe(void) {
    AMediaFormat *p = AMediaFormat_new();
    AMediaFormat_setInt32(p, "request-sync", 0);
    AMediaCodec_setParameters(codec, p);
    AMediaFormat_delete(p);
}

/* MTK's encoder doesn't always set the keyframe flag; also look for an IDR NAL unit (type 5) */
static int is_keyframe(const uint8_t *d, size_t n) {
    for (size_t i = 0; i + 3 < n && i < 256; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 31) == 5) return 1;
    return 0;
}

/* encoder output -> every client; a new client starts at a keyframe, preceded by SPS/PPS */
static void *encoder_output(void *arg) {
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(codec, &info, 500000);
        if (i < 0) continue;
        size_t cap; uint8_t *b = AMediaCodec_getOutputBuffer(codec, i, &cap);
        if (b && info.size > 0) {
            uint8_t *d = b + info.offset;
            pthread_mutex_lock(&mu);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                free(sps_pps); sps_pps = malloc(info.size);
                if (sps_pps) { memcpy(sps_pps, d, info.size); sps_pps_len = info.size; }
            } else {
                int key = (info.flags & 1) || is_keyframe(d, info.size);
                for (int k = 0; k < MAX_CLIENTS; k++) {
                    if (client_fd[k] <= 0) continue;
                    if (!client_ready[k]) {
                        if (!key) continue;
                        if (sps_pps && send_all(client_fd[k], sps_pps, sps_pps_len)) { close(client_fd[k]); client_fd[k] = 0; continue; }
                        client_ready[k] = 1;
                    }
                    if (send_all(client_fd[k], d, info.size)) { close(client_fd[k]); client_fd[k] = 0; }
                }
            }
            pthread_mutex_unlock(&mu);
        }
        AMediaCodec_releaseOutputBuffer(codec, i, false);
    }
    return NULL;
}

static ANativeWindow *create_encoder(int w, int h, int fps, int bps) {
    /* by name: createEncoderByType makes Android enumerate every codec first (~6 s) */
    static const char *const names[] = { "OMX.MTK.VIDEO.ENCODER.AVC",   /* Android 9 (M7, OMX) */
                                         "c2.mtk.avc.encoder" };         /* Android 12 (M8, Codec2) */
    for (size_t i = 0; !codec && i < sizeof names / sizeof names[0]; i++) codec = AMediaCodec_createCodecByName(names[i]);
    if (!codec) codec = AMediaCodec_createEncoderByType("video/avc");
    if (!codec) { fprintf(stderr, "no H.264 encoder\n"); exit(1); }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, bps);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F000789);   /* COLOR_FormatSurface */
    media_status_t m = AMediaCodec_configure(codec, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(f);
    if (m != AMEDIA_OK) { fprintf(stderr, "encoder configure: %d\n", m); exit(1); }
    ANativeWindow *win = NULL;
    if (AMediaCodec_createInputSurface(codec, &win) != AMEDIA_OK) { fprintf(stderr, "encoder input surface failed\n"); exit(1); }
    AMediaCodec_start(codec);
    return win;
}

void start_binder_threads(void);   /* common/binderpool.cpp */

/* drop clients that closed the connection (otherwise the camera stays on without a viewer) */
static void reap_clients(void) {
    pthread_mutex_lock(&mu);
    for (int k = 0; k < MAX_CLIENTS; k++) {
        char c; if (client_fd[k] <= 0) continue;
        ssize_t r = recv(client_fd[k], &c, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close(client_fd[k]); client_fd[k] = 0; }
    }
    pthread_mutex_unlock(&mu);
}

/* Only root may read the camera (the RTSP server on the Linux side); Android apps have their own uid.
   SO_PEERCRED doesn't work for TCP, so look up the uid of the client's end in /proc/net/tcp,
   matching all four of local/remote address and port. */
static int client_is_root(int fd) {
    struct sockaddr_in peer, me; socklen_t l = sizeof peer, lm = sizeof me;
    if (getpeername(fd, (struct sockaddr *)&peer, &l) || getsockname(fd, (struct sockaddr *)&me, &lm)) return 0;
    if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 0;
    unsigned cport = ntohs(peer.sin_port), sport = ntohs(me.sin_port); char row[512]; int ok = 0;
    FILE *f = fopen("/proc/net/tcp", "r"); if (!f) return 0;
    while (fgets(row, sizeof row, f)) {
        unsigned la, lp, ra, rp, uid;
        if (sscanf(row, " %*d: %8X:%4X %8X:%4X %*s %*s %*s %*s %u", &la, &lp, &ra, &rp, &uid) == 5 &&
            la == 0x0100007F && lp == cport && ra == 0x0100007F && rp == sport) { ok = (uid == 0); break; }
    }
    fclose(f); return ok;
}

static int client_count(void) {
    int n = 0; pthread_mutex_lock(&mu);
    for (int k = 0; k < MAX_CLIENTS; k++) if (client_fd[k] > 0) n++;
    pthread_mutex_unlock(&mu); return n;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: m7cam <camera-id> <port> <width> <height>\n"); return 2; }
    const char *id = argv[1];
    int port = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
    int bps = getenv("M7CAM_BPS") ? atoi(getenv("M7CAM_BPS")) : 2500000;
    signal(SIGPIPE, SIG_IGN);
    start_binder_threads();

    ACameraManager *man = ACameraManager_create();
    ACameraIdList *ids = NULL;
    if (ACameraManager_getCameraIdList(man, &ids) != ACAMERA_OK) { fprintf(stderr, "no camera service\n"); return 1; }
    fprintf(stderr, "cameras: %d\n", ids->numCameras);
    ACameraManager_deleteCameraIdList(ids);

    int ls = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) { perror("bind"); return 1; }
    struct timeval tick = { 1, 0 }; setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tick, sizeof tick);

    ANativeWindow *win = create_encoder(w, h, 30, bps);
    pthread_t t; pthread_create(&t, NULL, encoder_output, NULL);
    ACameraDevice *dev = NULL; ACameraCaptureSession *ses = NULL; ACaptureSessionOutputContainer *outs = NULL;
    ACaptureSessionOutput *out = NULL; ACameraOutputTarget *target = NULL; ACaptureRequest *req = NULL;
    int idle = 0;
    fprintf(stderr, "H.264 on 127.0.0.1:%d, camera %s %dx%d, waiting for a client\n", port, id, w, h);

    for (;;) {
        int fd = accept(ls, NULL, NULL);               /* returns every second (SO_RCVTIMEO) */
        if (fd >= 0 && !client_is_root(fd)) { fprintf(stderr, "client refused (not root)\n"); close(fd); fd = -1; }
        if (fd >= 0) {
            struct timeval tv = { 3, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            pthread_mutex_lock(&mu);
            int k; for (k = 0; k < MAX_CLIENTS && client_fd[k] > 0; k++);
            if (k < MAX_CLIENTS) { client_fd[k] = fd; client_ready[k] = 0; } else close(fd);
            pthread_mutex_unlock(&mu);
            if (camera_on && k < MAX_CLIENTS) request_keyframe();
        }
        reap_clients();
        int n = client_count();
        if (n > 0 && !camera_on) {
            ACameraDevice_StateCallbacks dc = { NULL, dev_disconnected, dev_error };
            camera_status_t s = ACameraManager_openCamera(man, id, &dc, &dev);
            if (s != ACAMERA_OK) { fprintf(stderr, "openCamera %s: %d\n", id, s); return 1; }
            ACaptureSessionOutputContainer_create(&outs); ACaptureSessionOutput_create(win, &out);
            ACaptureSessionOutputContainer_add(outs, out); ACameraOutputTarget_create(win, &target);
            /* TEMPLATE_RECORD delivers no frames into the encoder surface on this HAL */
            ACameraDevice_createCaptureRequest(dev, TEMPLATE_PREVIEW, &req); ACaptureRequest_addTarget(req, target);
            ACameraCaptureSession_stateCallbacks sc = { NULL, session_cb, session_cb, session_cb };
            if (ACameraDevice_createCaptureSession(dev, outs, &sc, &ses) != ACAMERA_OK ||
                ACameraCaptureSession_setRepeatingRequest(ses, NULL, 1, &req, NULL) != ACAMERA_OK) { fprintf(stderr, "capture session failed\n"); return 1; }
            camera_on = 1; idle = 0; request_keyframe();
            fprintf(stderr, "camera on (%d client%s)\n", n, n == 1 ? "" : "s");
        } else if (n == 0 && camera_on && ++idle >= 30) {
            ACameraCaptureSession_stopRepeating(ses); ACameraCaptureSession_close(ses);
            ACameraDevice_close(dev);
            camera_on = 0;
            ACaptureRequest_free(req); ACameraOutputTarget_free(target);
            ACaptureSessionOutputContainer_free(outs); ACaptureSessionOutput_free(out);
            fprintf(stderr, "camera off\n");
        } else if (n > 0) idle = 0;
    }
}
