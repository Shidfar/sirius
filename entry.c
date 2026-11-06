/*
 * entry.c - turns a finished recording into the two encrypted files that get uploaded, and uploads them.
 *
 * Nothing readable is ever written to disk: the recording is decrypted into a pipe, and what comes out the other
 * end (Opus audio, text) is encrypted to the owner's age public key before it touches a file. Only when both
 * encrypted files are complete is the locally encrypted recording deleted.
 */
#define _GNU_SOURCE
/* Paths are built with snprintf into 1 KB buffers from names that are far shorter; a too-long one would only fail to open. */
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "age.h"
#include "diary.h"
#include "entry.h"
#include "gigaam.h"
#include "oggopus.h"
#include "s3.h"

#define MAX_TRIES 3                                 /* an entry that keeps failing is left alone after this */
#define MIN_SAMPLES 8000                            /* shorter than half a second: an accidental tap, dropped */

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void entries_root(const Config *c, char *buf, size_t n) { snprintf(buf, n, "%s/entries", c->data_dir); }

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; if (mkdir(tmp, mode) && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(tmp, mode) && errno != EEXIST ? -1 : 0;
}

static void sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { fsync(fd); close(fd); }
}

/* Names of the entry directories, oldest first. The caller frees the array and its strings. */
static int list_entries(const Config *c, char ***names) {
    char root[1024];
    entries_root(c, root, sizeof root);
    struct dirent **ents;
    int n = scandir(root, &ents, NULL, alphasort);
    *names = NULL;
    if (n < 0) return 0;
    char **out = calloc((size_t)n + 1, sizeof *out);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (ents[i]->d_name[0] >= '0' && ents[i]->d_name[0] <= '9') out[k++] = strdup(ents[i]->d_name);
        free(ents[i]);
    }
    free(ents);
    *names = out;
    return k;
}

static void free_names(char **names, int n) {
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

int entry_create(const Config *c, char *id, size_t idn, char *dir, size_t dirn) {
    char root[1024];
    entries_root(c, root, sizeof root);
    if (mkdir_p(c->data_dir, 0700) || mkdir_p(root, 0700)) return -1;
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char base[32];
    strftime(base, sizeof base, "%Y%m%dT%H%M%SZ", &tm);
    for (int i = 0; i < 100; i++) {
        if (i) snprintf(id, idn, "%s-%d", base, i + 1);
        else snprintf(id, idn, "%s", base);
        snprintf(dir, dirn, "%s/%s", root, id);
        if (mkdir(dir, 0700) == 0) { sync_dir(root); return 0; }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

void store_counts(const Config *c, StoreCounts *n) {
    memset(n, 0, sizeof *n);
    char **names;
    int count = list_entries(c, &names);
    for (int i = 0; i < count; i++) {
        char p[1024], dir[1024];
        snprintf(dir, sizeof dir, "%s/entries/%s", c->data_dir, names[i]);
        snprintf(p, sizeof p, "%s/audio.enc", dir);
        if (exists(p)) {
            if (diary_in_use(p)) n->recording++;
            else {
                char t[1024];
                snprintf(t, sizeof t, "%s/tries", dir);
                FILE *f = fopen(t, "r");
                int tries = 0;
                if (f) { if (fscanf(f, "%d", &tries) != 1) tries = 0; fclose(f); }
                if (tries >= MAX_TRIES) n->stuck++; else n->to_transcribe++;
            }
        }
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d));) {
            size_t len = strlen(e->d_name);
            if (len > 4 && !strcmp(e->d_name + len - 4, ".age")) n->to_upload++;
        }
        if (d) closedir(d);
    }
    free_names(names, count);
}

/* ------------------------------------------------------ transcription -- */

struct CatJob { const char *path; int wfd; int rc; pthread_t th; };

static void *cat_thread(void *a) {
    struct CatJob *j = a;
    j->rc = diary_cat(j->path, j->wfd);
    close(j->wfd);
    return NULL;
}

/* Decrypts the recording into a pipe from a thread; returns the read end. */
static int start_cat(struct CatJob *j, const char *path) {
    int p[2];
    if (pipe(p)) return -1;
    j->path = path;
    j->wfd = p[1];
    j->rc = 0;
    if (pthread_create(&j->th, NULL, cat_thread, j)) { close(p[0]); close(p[1]); return -1; }
    return p[0];
}

/* Closes the read end (so a writer that is still busy fails) and returns diary_cat's result. */
static int finish_cat(struct CatJob *j, int rfd) {
    close(rfd);
    pthread_join(j->th, NULL);
    return j->rc;
}

static int sink_age(void *ctx, const void *data, size_t n) { return age_write(ctx, data, n) ? errno : 0; }
static ssize_t cookie_write(void *ctx, const char *buf, size_t n) { return age_write(ctx, buf, n) ? -1 : (ssize_t)n; }

static int open_part(const char *final_path, char *part, size_t n) {
    snprintf(part, n, "%s.part", final_path);
    unlink(part);
    return open(part, O_WRONLY | O_CREAT | O_EXCL, 0600);
}

/* Pass 1: the recording as an Opus file, age-encrypted. Returns the number of samples, or -1. */
static long encode_audio(const Config *c, const char *enc, const char *final_path, int *cat_rc) {
    struct CatJob job;
    int rfd = start_cat(&job, enc);
    if (rfd < 0) return -1;
    char part[1024];
    int out = open_part(final_path, part, sizeof part);
    AgeW *aw = out >= 0 ? age_open(out, c->recipient) : NULL;
    OggOpus *oo = aw ? oggopus_open(sink_age, aw, 24000) : NULL;
    if (!oo) { if (aw) age_close(aw); if (out >= 0) close(out); finish_cat(&job, rfd); return -1; }

    long samples = 0;
    int bad = 0;
    unsigned char buf[16384];
    size_t have = 0;
    ssize_t r;
    while (!bad && (r = read(rfd, buf + have, sizeof buf - have)) != 0) {
        if (r < 0) { if (errno == EINTR) continue; bad = 1; break; }
        have += (size_t)r;
        size_t whole = have / 2;
        if (oggopus_write(oo, (int16_t *)buf, whole)) bad = 1;
        samples += (long)whole;
        have -= whole * 2;
        if (have) memmove(buf, buf + whole * 2, have);
    }
    if (oggopus_close(oo)) bad = 1;
    if (age_close(aw)) bad = 1;
    if (fsync(out) || close(out)) bad = 1;
    *cat_rc = finish_cat(&job, rfd);
    if (bad) { unlink(part); return -1; }
    return samples;
}

/* Pass 2: the transcript, age-encrypted. */
static int write_transcript(const Config *c, Engine *E, const char *id, long samples, int cat_rc, const char *enc,
                            const char *final_path) {
    struct CatJob job;
    int rfd = start_cat(&job, enc);
    if (rfd < 0) return -1;
    char part[1024];
    int outfd = open_part(final_path, part, sizeof part);
    AgeW *aw = outfd >= 0 ? age_open(outfd, c->recipient) : NULL;
    FILE *out = aw ? fopencookie(aw, "w", (cookie_io_functions_t){.write = cookie_write}) : NULL;
    if (!out) { if (aw) age_close(aw); if (outfd >= 0) close(outfd); finish_cat(&job, rfd); return -1; }

    unsigned secs = (unsigned)(samples / 16000);
    fprintf(out, "Sirius diary entry %s, %u min %02u s\n", id, secs / 60, secs % 60);
    if (cat_rc == 3) fprintf(out, "(the recording was cut short, probably by a power loss)\n");
    if (cat_rc == 4) fprintf(out, "(part of the recording was damaged and is missing)\n");
    fprintf(out, "\n");
    gigaam_stream(E, rfd, out, 1, c->gate, 20, 0);
    int bad = fclose(out) != 0;                     /* flushes into the age stream */
    if (age_close(aw)) bad = 1;
    if (fsync(outfd) || close(outfd)) bad = 1;
    finish_cat(&job, rfd);
    if (bad) { unlink(part); return -1; }
    return 0;
}

static int bump_tries(const char *dir) {
    char p[1024];
    snprintf(p, sizeof p, "%s/tries", dir);
    int tries = 0;
    FILE *f = fopen(p, "r");
    if (f) { if (fscanf(f, "%d", &tries) != 1) tries = 0; fclose(f); }
    f = fopen(p, "w");
    if (f) { fprintf(f, "%d\n", tries + 1); fflush(f); fsync(fileno(f)); fclose(f); }
    return tries;                                   /* the number of earlier attempts */
}

static void remove_entry(const char *dir) {
    DIR *d = opendir(dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    if (d) closedir(d);
    rmdir(dir);
}

int store_transcribe(const Config *c, char *err, size_t errn) {
    char **names;
    int count = list_entries(c, &names), done = 0;
    Engine *E = NULL;
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < count; i++) {
        char dir[1024], enc[1024], a_final[1024], t_final[1024];
        snprintf(dir, sizeof dir, "%s/entries/%s", c->data_dir, names[i]);
        snprintf(enc, sizeof enc, "%s/audio.enc", dir);
        snprintf(a_final, sizeof a_final, "%s/audio.opus.age", dir);
        snprintf(t_final, sizeof t_final, "%s/transcript.txt.age", dir);
        if (!exists(enc) || diary_in_use(enc)) continue;
        if (!c->recipient[0]) { snprintf(err, errn, "AGE_RECIPIENT is not set"); free_names(names, count); return -1; }
        if (bump_tries(dir) >= MAX_TRIES) continue;

        int cat_rc = 0;
        long samples = encode_audio(c, enc, a_final, &cat_rc);
        if (samples < 0) { logf_("%s: cannot encode the audio", names[i]); continue; }
        if (cat_rc == 1) { logf_("%s: not a readable recording", names[i]); continue; }
        if (samples < MIN_SAMPLES) {                /* nothing worth keeping */
            logf_("%s: under half a second of audio, dropped", names[i]);
            remove_entry(dir);
            continue;
        }
        if (!E) E = gigaam_open(c->models_dir[0] ? c->models_dir : NULL, c->threads);
        if (write_transcript(c, E, names[i], samples, cat_rc, enc, t_final)) { logf_("%s: cannot write the transcript", names[i]); continue; }

        char a_part[1024], t_part[1024];
        snprintf(a_part, sizeof a_part, "%s.part", a_final);
        snprintf(t_part, sizeof t_part, "%s.part", t_final);
        if (rename(a_part, a_final) || rename(t_part, t_final)) { logf_("%s: rename failed: %s", names[i], strerror(errno)); continue; }
        sync_dir(dir);
        unlink(enc);                                /* only now is the locally decryptable copy gone */
        char tries[1024];
        snprintf(tries, sizeof tries, "%s/tries", dir);
        unlink(tries);
        sync_dir(dir);
        logf_("%s: done (%u s of audio%s)", names[i], (unsigned)(samples / 16000), cat_rc ? ", damaged" : "");
        done++;
    }
    if (E) gigaam_close(E);
    free_names(names, count);
    return done;
}

/* -------------------------------------------------------------- upload -- */

int store_upload(const Config *c, int *sent, char *err, size_t errn) {
    *sent = 0;
    const char *missing = config_missing(c);
    if (missing) { snprintf(err, errn, "%s", missing); return S3_FAILED; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    S3Conf s3 = {c->bucket, c->region, c->access_key, c->secret_key, c->endpoint[0] ? c->endpoint : NULL,
                 c->ca_file[0] ? c->ca_file : NULL};
    char **names;
    int count = list_entries(c, &names), result = S3_OK;
    for (int i = 0; i < count && result == S3_OK; i++) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/entries/%s", c->data_dir, names[i]);
        static const char *const files[] = {"transcript.txt.age", "audio.opus.age"};   /* the small one first */
        for (size_t k = 0; k < sizeof files / sizeof *files && result == S3_OK; k++) {
            char path[1024], key[1024];
            snprintf(path, sizeof path, "%s/%s", dir, files[k]);
            if (!exists(path)) continue;
            snprintf(key, sizeof key, "%s%s/%s", c->prefix, names[i], files[k]);
            result = s3_put_file(&s3, key, path, err, errn);
            if (result == S3_OK) { unlink(path); (*sent)++; }
        }
        if (result == S3_OK) { sync_dir(dir); char t[1024]; snprintf(t, sizeof t, "%s/audio.enc", dir); if (!exists(t)) rmdir(dir); }
    }
    free_names(names, count);
    return result;
}

/* ------------------------------------------------------------ commands -- */

static int load(Config *c) {
    char err[300];
    if (config_load(c, NULL, err, sizeof err)) { fprintf(stderr, "sirius: %s\n", err); return -1; }
    return 0;
}

int sync_main(int argc, char **argv) {
    (void)argc; (void)argv;
    Config c;
    if (load(&c)) return 1;
    char err[400] = "";
    int n = store_transcribe(&c, err, sizeof err);
    if (n < 0) { fprintf(stderr, "sirius: %s\n", err); return 1; }
    int sent;
    int rc = store_upload(&c, &sent, err, sizeof err);
    fprintf(stderr, "transcribed %d, uploaded %d file(s)", n, sent);
    if (rc == S3_OK) fprintf(stderr, "\n");
    else fprintf(stderr, "; %s: %s\n", rc == S3_RETRY ? "offline or busy, will retry" : "refused", err);
    return rc == S3_FAILED ? 1 : 0;
}

int status_main(int argc, char **argv) {
    (void)argc; (void)argv;
    Config c;
    if (load(&c)) return 1;
    StoreCounts n;
    store_counts(&c, &n);
    printf("recording now: %d\nwaiting for transcription: %d\nwaiting for upload (files): %d\nstuck: %d\n",
           n.recording, n.to_transcribe, n.to_upload, n.stuck);
    return 0;
}
