#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"

static void set(char *dst, size_t n, const char *v) { snprintf(dst, n, "%s", v); }

int config_load(Config *c, const char *path, char *err, size_t errn) {
    memset(c, 0, sizeof *c);
    set(c->data_dir, sizeof c->data_dir, "/var/lib/sirius");
    set(c->prefix, sizeof c->prefix, "entries/");
    c->gate = 0.005;
    if (!path) path = getenv("SIRIUS_CONF") ? getenv("SIRIUS_CONF") : "/etc/sirius.conf";
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        snprintf(err, errn, "%s: %s", path, strerror(errno));
        return -1;
    }
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) { snprintf(err, errn, "%s:%d: expected KEY=VALUE", path, lineno); fclose(f); return -1; }
        *eq = 0;
        char *v = eq + 1;
        for (char *e = s + strlen(s); e > s && isspace((unsigned char)e[-1]);) *--e = 0;
        while (isspace((unsigned char)*v)) v++;
        for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]);) *--e = 0;
        if (!strcmp(s, "DATA_DIR")) set(c->data_dir, sizeof c->data_dir, v);
        else if (!strcmp(s, "AGE_RECIPIENT")) set(c->recipient, sizeof c->recipient, v);
        else if (!strcmp(s, "S3_BUCKET")) set(c->bucket, sizeof c->bucket, v);
        else if (!strcmp(s, "S3_REGION")) set(c->region, sizeof c->region, v);
        else if (!strcmp(s, "S3_ACCESS_KEY")) set(c->access_key, sizeof c->access_key, v);
        else if (!strcmp(s, "S3_SECRET_KEY")) set(c->secret_key, sizeof c->secret_key, v);
        else if (!strcmp(s, "S3_PREFIX")) set(c->prefix, sizeof c->prefix, v);
        else if (!strcmp(s, "S3_ENDPOINT")) set(c->endpoint, sizeof c->endpoint, v);
        else if (!strcmp(s, "S3_CA_FILE")) set(c->ca_file, sizeof c->ca_file, v);
        else if (!strcmp(s, "MIC")) set(c->mic, sizeof c->mic, v);
        else if (!strcmp(s, "EVDEV")) set(c->evdev, sizeof c->evdev, v);
        else if (!strcmp(s, "MODELS_DIR")) set(c->models_dir, sizeof c->models_dir, v);
        else if (!strcmp(s, "GATE")) c->gate = atof(v) > 0 ? atof(v) : 0.005;
        else if (!strcmp(s, "FLIP")) c->flip = atoi(v);
        else if (!strcmp(s, "THREADS")) c->threads = atoi(v);
        else { snprintf(err, errn, "%s:%d: unknown setting %s", path, lineno, s); fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

const char *config_missing(const Config *c) {
    if (!c->recipient[0]) return "AGE_RECIPIENT is not set (the public key the entries are encrypted to)";
    if (!c->bucket[0] || !c->region[0] || !c->access_key[0] || !c->secret_key[0])
        return "S3_BUCKET, S3_REGION, S3_ACCESS_KEY and S3_SECRET_KEY must all be set";
    return NULL;
}

int config_save(const Config *c, const char *path) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) return -1;
    fprintf(f, "# Sirius settings. Read when the service starts (sudo systemctl restart sirius).\n"
               "# This file holds the S3 secret key: keep it readable by root only.\n\n");
    fprintf(f, "AGE_RECIPIENT=%s\n", c->recipient);
    fprintf(f, "S3_BUCKET=%s\nS3_REGION=%s\nS3_ACCESS_KEY=%s\nS3_SECRET_KEY=%s\n", c->bucket, c->region, c->access_key, c->secret_key);
    fprintf(f, "S3_PREFIX=%s\n", c->prefix);
    if (c->endpoint[0]) fprintf(f, "S3_ENDPOINT=%s\n", c->endpoint);
    if (c->ca_file[0]) fprintf(f, "S3_CA_FILE=%s\n", c->ca_file);
    fprintf(f, "\n# Optional\n");
    fprintf(f, "%sMIC=%s\n", c->mic[0] ? "" : "#", c->mic[0] ? c->mic : "plughw:1,0");
    fprintf(f, "%sEVDEV=%s\n", c->evdev[0] ? "" : "#", c->evdev[0] ? c->evdev : "/dev/input/by-id/usb-...-event-kbd");
    fprintf(f, "%sFLIP=1\n", c->flip ? "" : "#");
    fprintf(f, "%sDATA_DIR=%s\n", strcmp(c->data_dir, "/var/lib/sirius") ? "" : "#", c->data_dir);
    fprintf(f, "%sGATE=%g\n", c->gate != 0.005 ? "" : "#", c->gate);
    fprintf(f, "%sTHREADS=%d\n", c->threads ? "" : "#", c->threads ? c->threads : 3);
    if (c->models_dir[0]) fprintf(f, "MODELS_DIR=%s\n", c->models_dir);
    if (fflush(f) || fsync(fd) || fclose(f)) { unlink(tmp); return -1; }
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
