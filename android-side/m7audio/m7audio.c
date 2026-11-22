/* m7audio: sound for the Lenovo Tab M7 in Linux mode, through Android's audioserver (AAudio, NDK).
 *
 *   m7audio volume <0-15>    set the stream volumes (without system_server they sit at -inf)
 *   m7audio play <socket> [volume]
 *                            read S16LE stereo 48 kHz from PulseAudio's simple-protocol unix socket and play
 *                            it; the output stream (and the amplifier) only opens on non-silent audio and
 *                            closes again after 3 s of silence. With a volume (0-15) it sets the stream volume
 *                            again every time it opens the speaker: if audioserver restarts, the volumes are
 *                            back at -inf, and nothing else would notice
 *   m7audio selftest         play 880 Hz while recording with the built-in microphone; print the tone level
 *   m7audio measure <s>      record for <s> seconds and print the 880 Hz level (start it before the playback)
 *
 * PERFORMANCE_MODE_NONE everywhere: never the MMAP path (mmap on this MTK PCM crashes the kernel).
 */
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define RATE 48000
#define BLOCK 960

void start_binder_threads(void);   /* common/binderpool.cpp */

static AAudioStream *open_stream(int input, int channels) {
    AAudioStreamBuilder *b; AAudioStream *s = NULL;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, RATE);
    AAudioStreamBuilder_setChannelCount(b, channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { fprintf(stderr, "open %s: %s\n", input ? "microphone" : "speaker", AAudio_convertResultToText(r)); return NULL; }
    fprintf(stderr, "%s: %d Hz, %d channels\n", input ? "microphone" : "speaker", AAudioStream_getSampleRate(s), AAudioStream_getChannelCount(s));
    return s;
}

/* Goertzel at 880 Hz, plus RMS */
static void analyse(const int16_t *x, int n, double *tone, double *rms) {
    double k = 2 * cos(2 * M_PI * 880 / RATE), a = 0, b = 0, q = 0;
    for (int i = 0; i < n; i++) { double t = x[i] + k * a - b; b = a; a = t; q += (double)x[i] * x[i]; }
    *tone = sqrt(fmax(0, a * a + b * b - k * a * b)) / n; *rms = sqrt(q / n);
}

static void record(AAudioStream *in, int16_t *rec, int n) {
    for (int got = 0; got < n;) {
        int r = AAudioStream_read(in, rec + got, n - got > BLOCK ? BLOCK : n - got, 1000000000LL);
        if (r < 0) { fprintf(stderr, "read: %s\n", AAudio_convertResultToText(r)); break; }
        got += r;
    }
}

static int measure(int sec) {
    AAudioStream *in = open_stream(1, 1); if (!in) return 1;
    AAudioStream_requestStart(in);
    int n = RATE * sec; int16_t *rec = calloc(n, 2); double tone, rms;
    record(in, rec, n);
    analyse(rec, n, &tone, &rms); printf("recorded %d s: tone %.1f rms %.1f\n", sec, tone, rms);
    AAudioStream_requestStop(in); AAudioStream_close(in); free(rec); return 0;
}

static int selftest(void) {
    AAudioStream *in = open_stream(1, 1); if (!in) return 1;
    AAudioStream_requestStart(in);
    int n = RATE * 2; int16_t *rec = calloc(n, 2), block[BLOCK * 2]; double tone, rms;
    record(in, rec, n);
    analyse(rec + RATE / 4, n - RATE / 4, &tone, &rms); printf("silence:   tone %.1f rms %.1f\n", tone, rms);
    AAudioStream *out = open_stream(0, 2); if (!out) return 1;
    AAudioStream_requestStart(out);
    long phase = 0; int got = 0;
    while (got < n) {
        for (int i = 0; i < BLOCK; i++, phase++) { int16_t v = 12000 * sin(2 * M_PI * 880 * phase / RATE); block[2 * i] = block[2 * i + 1] = v; }
        AAudioStream_write(out, block, BLOCK, 1000000000LL);
        int r = AAudioStream_read(in, rec + got, n - got > BLOCK ? BLOCK : n - got, 0); if (r > 0) got += r;
    }
    analyse(rec + RATE / 4, n - RATE / 4, &tone, &rms); printf("with tone: tone %.1f rms %.1f\n", tone, rms);
    AAudioStream_requestStop(out); AAudioStream_close(out); AAudioStream_requestStop(in); AAudioStream_close(in); free(rec);
    return 0;
}

static int silent(const int16_t *x, size_t n) {
    for (size_t i = 0; i < n; i++) if (x[i] > 2 || x[i] < -2) return 0;
    return 1;
}

static int set_volume(int index);

/* Microphone -> FIFO (PulseAudio's module-pipe-source in the chroot reads it as a normal source).
 * Mono, 16-bit, 48 kHz. Reopens the microphone when audioserver restarts or the reader goes away. */
static int mic(const char *path) {
    int16_t buf[BLOCK];
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) { sleep(2); continue; }
        AAudioStream *in = open_stream(1, 1);
        if (!in) { close(fd); sleep(5); continue; }
        AAudioStream_requestStart(in);
        fprintf(stderr, "microphone -> %s\n", path);
        for (;;) {
            aaudio_result_t r = AAudioStream_read(in, buf, BLOCK, 1000000000LL);
            if (r < 0) { fprintf(stderr, "microphone: %s\n", AAudio_convertResultToText(r)); break; }
            if (r > 0 && write(fd, buf, (size_t)r * 2) < 0) { fprintf(stderr, "microphone: reader gone\n"); break; }
        }
        AAudioStream_requestStop(in); AAudioStream_close(in); close(fd); sleep(1);
    }
}

static int play(const char *path, int volume) {
    int16_t buf[BLOCK * 2]; AAudioStream *out = NULL; int quiet_blocks = 0;
    for (;;) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
        if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); sleep(2); continue; }
        fprintf(stderr, "connected to %s\n", path);
        size_t fill = 0;
        for (;;) {
            ssize_t k = recv(fd, (char *)buf + fill, sizeof buf - fill, 0);
            if (k <= 0) break;
            fill += k;
            size_t frames = fill / 4;
            if (!frames) continue;
            if (silent(buf, frames * 2)) {
                if (out && ++quiet_blocks > 3 * RATE / BLOCK) { AAudioStream_requestStop(out); AAudioStream_close(out); out = NULL; fprintf(stderr, "silence: speaker closed\n"); }
            } else quiet_blocks = 0;
            if (!out && !quiet_blocks) {
                if (volume >= 0) set_volume(volume);
                out = open_stream(0, 2); if (out) AAudioStream_requestStart(out);
            }
            if (out && AAudioStream_write(out, buf, frames, 1000000000LL) < 0) { AAudioStream_close(out); out = NULL; }
            memmove(buf, (char *)buf + frames * 4, fill - frames * 4); fill -= frames * 4;
        }
        close(fd);
        if (out) { AAudioStream_requestStop(out); AAudioStream_close(out); out = NULL; }
        sleep(1);
    }
}

/* Without system_server nobody initialises the stream volumes; set them through AudioSystem (libaudioclient). */
static int set_volume(int index) {
    void *lib = dlopen("libaudioclient.so", RTLD_NOW);
    int (*init)(int, int, int) = lib ? dlsym(lib, "_ZN7android11AudioSystem16initStreamVolumeE19audio_stream_type_tii") : NULL;
    /* Android 9 (M7): device as unsigned int; Android 12+ (M8): its own enum type audio_devices_t. Same ABI. */
    int (*set)(int, int, unsigned) = lib ? dlsym(lib, "_ZN7android11AudioSystem20setStreamVolumeIndexE19audio_stream_type_tij") : NULL;
    if (lib && !set) set = dlsym(lib, "_ZN7android11AudioSystem20setStreamVolumeIndexE19audio_stream_type_ti15audio_devices_t");
    if (!init || !set) { fprintf(stderr, "libaudioclient not usable\n"); return 1; }
    int r = 0;
    /* Only AUDIO_STREAM_MUSIC (3): everything from Linux plays through AAudio with usage MEDIA, i.e. the music
       stream. Initialising more streams broke the M8 (Android 12): afterwards the next recording made the MTK
       audio HAL hang, and audioserver aborted on a TimeCheck timeout (forgetting every volume again). */
    const int stream = 3;
    init(stream, 0, 15);
    r |= set(stream, index, 0x2);                           /* AUDIO_DEVICE_OUT_SPEAKER */
    r |= set(stream, index, 0x40000000);                    /* AUDIO_DEVICE_OUT_DEFAULT */
    fprintf(stderr, "volume %d/15 (%d)\n", index, r);
    return r != 0;
}

int main(int argc, char **argv) {
    start_binder_threads();
    if (argc > 2 && !strcmp(argv[1], "volume")) return set_volume(atoi(argv[2]));
    if (argc > 2 && !strcmp(argv[1], "play")) return play(argv[2], argc > 3 ? atoi(argv[3]) : -1);
    if (argc > 2 && !strcmp(argv[1], "mic")) return mic(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "selftest")) return selftest();
    if (argc > 2 && !strcmp(argv[1], "measure")) return measure(atoi(argv[2]));
    fprintf(stderr, "usage: m7audio volume <0-15> | play <socket> | mic <fifo> | selftest | measure <seconds>\n");
    return 2;
}
