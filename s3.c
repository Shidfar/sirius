/*
 * s3.c - PUT a file to S3. libcurl signs the request (AWS Signature V4) and talks TLS through mbedTLS; the Amazon
 * root certificates are built into the binary, so the device needs no certificate store.
 * The bytes are age-encrypted before they get here: the bucket only ever holds ciphertext.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <sodium.h>

#include "embed.h"
#include "s3.h"

struct Reply { char text[512]; size_t n; };

static size_t keep_reply(char *p, size_t sz, size_t nm, void *u) {
    struct Reply *r = u;
    size_t n = sz * nm;
    if (r->n < sizeof r->text - 1) {
        size_t take = n < sizeof r->text - 1 - r->n ? n : sizeof r->text - 1 - r->n;
        memcpy(r->text + r->n, p, take);
        r->n += take;
        r->text[r->n] = 0;
    }
    return n;
}

static size_t read_file(char *buf, size_t sz, size_t nm, void *u) { return fread(buf, sz, nm, (FILE *)u); }

/* SHA-256 of the file as lower-case hex, and its size. */
static int hash_file(const char *path, char hex[65], long long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    unsigned char buf[16384], sum[32];
    size_t r;
    long long total = 0;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) { crypto_hash_sha256_update(&st, buf, r); total += (long long)r; }
    int bad = ferror(f);
    fclose(f);
    crypto_hash_sha256_final(&st, sum);
    sodium_bin2hex(hex, 65, sum, 32);
    *size = total;
    return bad ? -1 : 0;
}

/* The text between <tag> and </tag> in an S3 error document, or NULL. */
static const char *xml_field(const char *xml, const char *tag, char *out, size_t n) {
    char open[32], close[32];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = strstr(xml, open), *b = a ? strstr(a, close) : NULL;
    if (!a || !b) return NULL;
    a += strlen(open);
    size_t len = (size_t)(b - a) < n - 1 ? (size_t)(b - a) : n - 1;
    memcpy(out, a, len);
    out[len] = 0;
    return out;
}

int s3_put_file(const S3Conf *c, const char *key, const char *path, char *err, size_t errn) {
    char hex[65];
    long long size;
    if (hash_file(path, hex, &size)) { snprintf(err, errn, "cannot read %s: %s", path, strerror(errno)); return S3_FAILED; }

    char url[1024], sign[128], userpwd[512], hdr[128];
    if (c->endpoint) snprintf(url, sizeof url, "%s/%s/%s", c->endpoint, c->bucket, key);               /* path style */
    else if (strchr(c->bucket, '.')) snprintf(url, sizeof url, "https://s3.%s.amazonaws.com/%s/%s", c->region, c->bucket, key);
    else snprintf(url, sizeof url, "https://%s.s3.%s.amazonaws.com/%s", c->bucket, c->region, key);     /* virtual host */
    snprintf(sign, sizeof sign, "aws:amz:%s:s3", c->region);
    snprintf(userpwd, sizeof userpwd, "%s:%s", c->access_key, c->secret_key);
    snprintf(hdr, sizeof hdr, "x-amz-content-sha256: %s", hex);

    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "cannot read %s: %s", path, strerror(errno)); return S3_FAILED; }
    CURL *h = curl_easy_init();
    if (!h) { fclose(f); snprintf(err, errn, "cannot start libcurl"); return S3_RETRY; }
    struct curl_slist *headers = curl_slist_append(NULL, hdr);
    headers = curl_slist_append(headers, "Expect:");               /* no 100-continue round trip */
    struct Reply reply = {{0}, 0};
    char curl_err[CURL_ERROR_SIZE] = "";

    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(h, CURLOPT_READFUNCTION, read_file);
    curl_easy_setopt(h, CURLOPT_READDATA, f);
    curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_AWS_SIGV4, sign);
    curl_easy_setopt(h, CURLOPT_USERPWD, userpwd);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, keep_reply);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &reply);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curl_err);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1000L);            /* under 1 KB/s for a minute: give up */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);                /* never send the signed request elsewhere */
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    if (c->ca_file) {
        curl_easy_setopt(h, CURLOPT_CAINFO, c->ca_file);
    } else {
        Blob roots = embedded("amazon-roots.pem");
        if (roots.p) {
            struct curl_blob b = {.data = (void *)roots.p, .len = roots.n, .flags = CURL_BLOB_NOCOPY};
            curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &b);
        } else {
            curl_easy_setopt(h, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");   /* development builds */
        }
    }

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    fclose(f);

    if (rc != CURLE_OK) {                                           /* never reached the server: offline, DNS, TLS, timeout */
        snprintf(err, errn, "%s", curl_err[0] ? curl_err : curl_easy_strerror(rc));
        return S3_RETRY;
    }
    if (status >= 200 && status < 300) return S3_OK;

    char code[64] = "", msg[200] = "";
    xml_field(reply.text, "Code", code, sizeof code);
    xml_field(reply.text, "Message", msg, sizeof msg);
    snprintf(err, errn, "HTTP %ld %s %s", status, code, msg);
    if (status >= 500 || status == 429 || status == 408 || !strcmp(code, "RequestTimeTooSkewed"))
        return S3_RETRY;                                            /* the clock may simply not be set yet */
    return S3_FAILED;
}

int s3_main(int argc, char **argv) {
    const char *bucket = getenv("S3_BUCKET"), *region = getenv("S3_REGION"), *ak = getenv("S3_ACCESS_KEY"),
               *sk = getenv("S3_SECRET_KEY");
    if (argc != 3 || !bucket || !region || !ak || !sk) {
        fprintf(stderr, "usage: S3_BUCKET=.. S3_REGION=.. S3_ACCESS_KEY=.. S3_SECRET_KEY=.. [S3_ENDPOINT=http://host:port] [S3_CA_FILE=..] %s FILE KEY\n", argv[0]);
        return 2;
    }
    S3Conf c = {bucket, region, ak, sk, getenv("S3_ENDPOINT"), getenv("S3_CA_FILE")};
    curl_global_init(CURL_GLOBAL_DEFAULT);
    char err[300] = "";
    int rc = s3_put_file(&c, argv[2], argv[1], err, sizeof err);
    if (rc == S3_OK) puts("uploaded");
    else fprintf(stderr, "%s: %s\n", rc == S3_RETRY ? "try again later" : "refused", err);
    return rc;
}
