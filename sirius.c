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
#include "diary.h"
#include "gigaam.h"
#include "oggopus.h"
#include "s3.h"

int epdtext_main(int argc, char **argv);

static int usage(void) {
    fprintf(stderr,
            "usage: sirius COMMAND [ARGUMENTS]\n"
            "  transcribe   speech to text: a WAV file, or -s for raw audio on stdin\n"
            "  show         show text on the e-Paper display\n"
            "  diary        encrypted local recordings: rec FILE, cat FILE\n"
            "  opus         compress raw audio on stdin to an Ogg Opus file\n"
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
    if (!strcmp(cmd, "s3put")) return sub(s3_main, cmd, argc, argv);
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) { puts("sirius 0.2"); return 0; }
    return usage();
}
