/*
 * sirius.c - the one program. Everything the voice diary needs is in this binary: speech recognition with its
 * models and ONNX Runtime, encryption, audio capture and compression, uploading, the e-Paper display.
 *
 *   sirius transcribe ...   speech to text (a WAV file, or a stream of raw audio)
 *   sirius show ...         put text on the e-Paper display
 *   sirius diary ...        the encrypted local recordings (rec / cat)
 *   sirius version
 */
#define _GNU_SOURCE
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <sodium.h>

#include "age.h"
#include "daemon.h"
#include "diary.h"
#include "entry.h"
#include "gigaam.h"
#include "install.h"
#include "oggopus.h"
#include "rec.h"
#include "s3.h"

int epdtext_main(int argc, char **argv);

static int usage(void) {
    fprintf(stderr,
            "usage: sirius COMMAND [ARGUMENTS]\n"
            "  transcribe   speech to text: a WAV file, or -s for raw audio on stdin\n"
            "  show         show text on the e-Paper display\n"
            "  diary        encrypted local recordings: rec FILE, cat FILE\n"
            "  opus         compress raw audio on stdin to an Ogg Opus file\n"
            "  install      set up a fresh Raspberry Pi (run once, as root)\n"
            "  setup        enter the age key and S3 details, test the upload (as root)\n"
            "  run          the diary: wait for the button, record, transcribe, upload (what the service runs)\n"
            "  ctl          toggle | sync: control the running diary like the button does\n"
            "  record       record from the microphone into an encrypted file (Ctrl-C to stop)\n"
            "  sync         transcribe the waiting recordings and upload what is ready\n"
            "  status       show what is waiting\n"
            "  s3put        upload a file to S3 (settings in S3_* environment variables)\n"
            "  age          encrypt stdin to an age public key (age1...), for checking a key\n"
            "  version\n");
    return 2;
}

/* Runs `fn` as if it were a program of its own named "sirius <name>". */
static int sub(int (*fn)(int, char **), const char *name, int argc, char **argv) {
    static char title[64];
    snprintf(title, sizeof title, "sirius %s", name);
    argv[1] = title;
    return fn(argc - 1, argv + 1);
}

int main(int argc, char **argv) {
#ifdef __GLIBC__
    /* Pin glibc's mmap threshold. Its dynamic growth (triggered by freeing any large buffer) makes
     * ONNX Runtime's big allocations stay resident: ~100 MB more peak memory, varying with the input. */
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
    if (sodium_init() < 0) errx(1, "cannot initialise libsodium");
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "transcribe")) return sub(gigaam_main, cmd, argc, argv);
    if (!strcmp(cmd, "show")) return sub(epdtext_main, cmd, argc, argv);
    if (!strcmp(cmd, "diary")) return sub(diary_main, cmd, argc, argv);
    if (!strcmp(cmd, "age")) return sub(age_main, cmd, argc, argv);
    if (!strcmp(cmd, "opus")) return sub(oggopus_main, cmd, argc, argv);
    if (!strcmp(cmd, "install")) return sub(install_main, cmd, argc, argv);
    if (!strcmp(cmd, "setup")) return sub(setup_main, cmd, argc, argv);
    if (!strcmp(cmd, "run")) return sub(daemon_main, cmd, argc, argv);
    if (!strcmp(cmd, "ctl")) return sub(ctl_main, cmd, argc, argv);
    if (!strcmp(cmd, "record")) return sub(rec_main, cmd, argc, argv);
    if (!strcmp(cmd, "sync")) return sub(sync_main, cmd, argc, argv);
    if (!strcmp(cmd, "status")) return sub(status_main, cmd, argc, argv);
    if (!strcmp(cmd, "s3put")) return sub(s3_main, cmd, argc, argv);
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) { puts("sirius 0.2"); return 0; }
    return usage();
}
