/*
 * rec.c - microphone to encrypted file. ALSA's "plughw" device takes care of the microphone's own sample rate,
 * channels and sample format, so any USB microphone gives us 16 kHz mono 16-bit.
 */
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "diary.h"
#include "rec.h"

#define RATE 16000
#define BLOCK 4000                                  /* frames per read: 250 ms */

const char *rec_find_mic(char *buf, size_t n) {
    for (int card = 0; card < 8; card++) {
        for (int dev = 0; dev < 4; dev++) {
            char path[96];
            struct stat st;
            snprintf(path, sizeof path, "/proc/asound/card%d/pcm%dc", card, dev);
            if (stat(path, &st) == 0) { snprintf(buf, n, "plughw:%d,%d", card, dev); return buf; }
        }
    }
    return NULL;
}

/* For tests without a microphone: SIRIUS_FAKE_MIC=file.pcm plays a raw 16 kHz mono file into the recording in real
 * time (silence once the file has ended). */
static int rec_fake(const char *pcm_path, const char *path, volatile sig_atomic_t *stop, char *err, size_t errn) {
    FILE *f = fopen(pcm_path, "rb");
    if (!f) { snprintf(err, errn, "%s: cannot open", pcm_path); return -1; }
    DiaryW *w = diary_create(path);
    int16_t buf[BLOCK];
    while (!*stop) {
        size_t got = fread(buf, sizeof *buf, BLOCK, f);
        memset(buf + got, 0, (BLOCK - got) * sizeof *buf);
        diary_write(w, buf, sizeof buf);
        struct timespec ts = {0, 250 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    fclose(f);
    diary_finish(w);
    return 0;
}

int rec_run(const char *name, const char *path, volatile sig_atomic_t *stop, char *err, size_t errn) {
    if (getenv("SIRIUS_FAKE_MIC")) return rec_fake(getenv("SIRIUS_FAKE_MIC"), path, stop, err, errn);
    char found[32];
    if (!name || !*name) name = rec_find_mic(found, sizeof found);
    if (!name) { snprintf(err, errn, "no microphone found (is the USB microphone plugged in?)"); return -1; }

    snd_pcm_t *pcm;
    int rc = snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) { snprintf(err, errn, "cannot open %s: %s", name, snd_strerror(rc)); return -1; }
    rc = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, RATE, 1, 500000);
    if (rc < 0) { snprintf(err, errn, "%s does not do 16 kHz mono: %s", name, snd_strerror(rc)); snd_pcm_close(pcm); return -1; }

    DiaryW *w = diary_create(path);
    int16_t buf[BLOCK];
    int result = 0;
    snd_pcm_prepare(pcm);
    while (!*stop) {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buf, BLOCK);
        if (got == -EINTR || got == -EAGAIN) continue;                       /* a signal: the loop condition decides */
        if (got == -EPIPE) { snd_pcm_prepare(pcm); continue; }              /* overrun: some audio was lost */
        if (got == -ESTRPIPE) { while ((rc = snd_pcm_resume(pcm)) == -EAGAIN) usleep(100000); snd_pcm_prepare(pcm); continue; }
        if (got < 0) {                                                       /* e.g. the microphone was unplugged */
            snprintf(err, errn, "the microphone stopped: %s", snd_strerror((int)got));
            result = -2;
            break;
        }
        diary_write(w, buf, (size_t)got * sizeof *buf);
    }
    snd_pcm_close(pcm);
    diary_finish(w);
    return result;
}

static volatile sig_atomic_t g_stop;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

/* `sirius record [-d DEVICE] FILE`: records until Ctrl-C into an encrypted diary file (`sirius diary cat FILE` plays it back). */
int rec_main(int argc, char **argv) {
    const char *dev = NULL;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-d")) { dev = argv[i + 1]; i += 2; }
    if (i != argc - 1) { fprintf(stderr, "usage: %s [-d ALSA_DEVICE] FILE   (stop with Ctrl-C)\n", argv[0]); return 2; }
    struct sigaction sa = {.sa_handler = on_stop};      /* no SA_RESTART: the signal interrupts the read */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    char err[300] = "";
    int rc = rec_run(dev, argv[i], &g_stop, err, sizeof err);
    if (rc) fprintf(stderr, "%s: %s\n", argv[0], err);
    return rc ? 1 : 0;
}
