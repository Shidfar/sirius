/*
 * diary.c - encrypted local storage for voice diary recordings.
 *
 *   sirius diary rec FILE    read raw PCM from stdin and write it, encrypted, to FILE while it arrives
 *   sirius diary cat FILE    decrypt FILE to stdout
 * and, for the rest of the program, diary.h (diary_create / diary_write / diary_finish / diary_cat).
 *
 * Exit codes of `diary cat` (and the return value of diary_cat): 0 complete, 3 cut short (a power cut: everything up to the last complete
 * chunk was written to stdout), 4 a chunk failed authentication (damaged or tampered file).
 *
 * The key is compiled into the program (localkey.h, made by `make`). It is only a speed bump for someone
 * who finds the SD card: anyone who has the card has the key. The audio only stays in this form between the
 * recording and its transcription; after that it is deleted and only copies encrypted to the owner's public
 * key remain (see process-entry.sh).
 *
 * File format: "SIRD1\n", the 24-byte secretstream header, then chunks of [u32 length, little endian]
 * [ciphertext]. Each chunk holds up to CHUNK bytes of audio and is flushed to disk as soon as it is written;
 * the last one carries the FINAL tag, so a cut-off file is told apart from a finished one.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

#include "diary.h"
#include "localkey.h"

#define MAGIC "SIRD1\n"
#define MAGIC_LEN 6
#define CHUNK (32 * 1024)                           /* 1 s of 16 kHz 16-bit mono audio */
#define ABYTES crypto_secretstream_xchacha20poly1305_ABYTES
#define HEADER_BYTES crypto_secretstream_xchacha20poly1305_HEADERBYTES

_Static_assert(sizeof LOCAL_KEY == crypto_secretstream_xchacha20poly1305_KEYBYTES, "LOCAL_KEY must be 32 bytes");

static volatile sig_atomic_t stop;

static void on_signal(int sig) { (void)sig; stop = 1; }

/* Reads up to n bytes, stopping early only at end of input or when a stop signal arrives. */
static size_t read_full(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) { if (stop) break; continue; }
        if (r < 0) err(1, "read");
        if (r == 0) break;
        got += (size_t)r;
    }
    return got;
}

static void write_full(int fd, const unsigned char *buf, size_t n) {
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) err(1, "write");
        buf += w;
        n -= (size_t)w;
    }
}

/* ------------------------------------------------------------- writer -- */

struct DiaryW {
    int fd;
    char *path;
    crypto_secretstream_xchacha20poly1305_state st;
    unsigned char plain[CHUNK];
    size_t fill;
};

DiaryW *diary_create(const char *path) {
    DiaryW *w = calloc(1, sizeof *w);
    if (!w) errx(1, "out of memory");
    w->fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (w->fd < 0) err(1, "%s", path);
    w->path = strdup(path);
    unsigned char header[HEADER_BYTES];
    crypto_secretstream_xchacha20poly1305_init_push(&w->st, header, LOCAL_KEY);
    write_full(w->fd, (const unsigned char *)MAGIC, MAGIC_LEN);
    write_full(w->fd, header, sizeof header);
    fdatasync(w->fd);
    return w;
}

static void push_chunk(DiaryW *w, int final) {
    unsigned char cipher[CHUNK + ABYTES + 4];
    unsigned long long clen;
    crypto_secretstream_xchacha20poly1305_push(&w->st, cipher + 4, &clen, w->plain, w->fill, NULL, 0,
        final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
    for (int i = 0; i < 4; i++) cipher[i] = (unsigned char)(clen >> (8 * i));
    write_full(w->fd, cipher, 4 + (size_t)clen);
    fdatasync(w->fd);                               /* at most one second of audio is lost in a power cut */
    w->fill = 0;
}

void diary_write(DiaryW *w, const void *pcm, size_t n) {
    const unsigned char *p = pcm;
    while (n) {
        size_t take = CHUNK - w->fill < n ? CHUNK - w->fill : n;
        memcpy(w->plain + w->fill, p, take);
        w->fill += take; p += take; n -= take;
        if (w->fill == CHUNK) push_chunk(w, 0);
    }
}

void diary_finish(DiaryW *w) {
    push_chunk(w, 1);
    if (close(w->fd)) err(1, "%s", w->path);
    sodium_memzero(w->plain, sizeof w->plain);
    free(w->path);
    free(w);
}

static int cmd_rec(const char *path) {
    struct sigaction sa = {.sa_handler = on_signal};  /* no SA_RESTART: a signal interrupts read() */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    DiaryW *w = diary_create(path);
    unsigned char buf[CHUNK];
    for (;;) {
        size_t n = read_full(STDIN_FILENO, buf, CHUNK);
        diary_write(w, buf, n);
        if (n < CHUNK || stop) break;               /* a short read means end of input (or a stop signal) */
    }
    diary_finish(w);
    sodium_memzero(buf, sizeof buf);
    return 0;
}

static int cat_fd(const char *path, int fd, int out_fd) {
    unsigned char magic[MAGIC_LEN], header[HEADER_BYTES];
    if (read_full(fd, magic, MAGIC_LEN) != MAGIC_LEN || memcmp(magic, MAGIC, MAGIC_LEN) ||
        read_full(fd, header, sizeof header) != sizeof header)
        { warnx("%s: not a diary file", path); return 1; }
    crypto_secretstream_xchacha20poly1305_state st;
    if (crypto_secretstream_xchacha20poly1305_init_pull(&st, header, LOCAL_KEY) != 0) { warnx("%s: bad header", path); return 1; }

    unsigned char cipher[CHUNK + ABYTES], plain[CHUNK], len4[4];
    unsigned long long total = 0;
    for (;;) {
        size_t got = read_full(fd, len4, 4);
        if (got == 0 && !stop) { warnx("%s: cut short after %llu bytes (no final chunk)", path, total); return 3; }
        uint32_t clen = (uint32_t)len4[0] | (uint32_t)len4[1] << 8 | (uint32_t)len4[2] << 16 | (uint32_t)len4[3] << 24;
        if (got < 4 || clen < ABYTES || clen > sizeof cipher) { warnx("%s: cut short after %llu bytes", path, total); return 3; }
        if (read_full(fd, cipher, clen) != clen) { warnx("%s: cut short after %llu bytes", path, total); return 3; }

        unsigned long long plen;
        unsigned char tag;
        if (crypto_secretstream_xchacha20poly1305_pull(&st, plain, &plen, &tag, cipher, clen, NULL, 0) != 0) {
            warnx("%s: damaged data after %llu bytes", path, total);
            return 4;
        }
        write_full(out_fd, plain, (size_t)plen);
        total += plen;
        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) return 0;
    }
}

int diary_cat(const char *path, int out_fd) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { warn("%s", path); return 1; }
    int rc = cat_fd(path, fd, out_fd);
    close(fd);
    return rc;
}

int diary_main(int argc, char **argv) {
    if (sodium_init() < 0) errx(1, "cannot initialise libsodium");
    if (argc == 3 && !strcmp(argv[1], "rec")) return cmd_rec(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "cat")) return diary_cat(argv[2], STDOUT_FILENO);
    fprintf(stderr, "usage: %s rec FILE   (raw 16 kHz mono 16-bit PCM on stdin)\n"
                    "       %s cat FILE   (decrypted PCM on stdout)\n", argv[0], argv[0]);
    return 2;
}
