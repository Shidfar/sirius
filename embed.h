/* embed.h - data that is built into the sirius binary (ONNX Runtime, the speech models, ...). */
#pragma once
#include <stddef.h>

typedef struct {
    const unsigned char *p;   /* NULL when the thing is not built in */
    size_t n;                 /* p[n] is a zero byte */
} Blob;

Blob embedded(const char *name);

/* Drops the blob's pages from this process's resident memory; they are re-read from the file if used again. */
void blob_release(Blob b);
