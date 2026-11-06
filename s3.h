/* s3.h - uploads files to an S3 bucket (or an S3-compatible server) with libcurl and AWS Signature V4. */
#pragma once

typedef struct {
    const char *bucket;
    const char *region;
    const char *access_key;
    const char *secret_key;
    const char *endpoint;     /* NULL for Amazon S3; "http://host:port" for a compatible server (path-style URLs) */
    const char *ca_file;      /* NULL: the Amazon root certificates built into the binary */
} S3Conf;

enum { S3_OK = 0, S3_RETRY = 1, S3_FAILED = 2 };

/* Uploads the file at `path` as object `key` with a single PUT. S3_RETRY: no connection, a timeout, a server error or
 * a wrong clock - try again later. S3_FAILED: the server refused it (bad keys, no permission, no such bucket); `err`
 * (size n) gets a short explanation in both failure cases. */
int s3_put_file(const S3Conf *c, const char *key, const char *path, char *err, size_t n);

/* `sirius s3put FILE KEY` with the S3_* settings from the environment, for testing the setup. */
int s3_main(int argc, char **argv);
