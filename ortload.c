/*
 * ortload.c - finds ONNX Runtime. In the single-file build the library is inside the binary: it is copied into
 * an anonymous memory file (memfd) and loaded from there, so nothing is written to disk and nothing has to be
 * installed. Development builds load libonnxruntime.so.1 from the system instead.
 * SIRIUS_ORT_LIB=/path/to/libonnxruntime.so overrides both.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "embed.h"
#include "ortload.h"

const OrtApiBase *sirius_ort_base(void) {
    static const OrtApiBase *base;
    if (base) return base;

    void *h;
    const char *over = getenv("SIRIUS_ORT_LIB");
    Blob lib = embedded("libonnxruntime.so");
    if (over) {
        h = dlopen(over, RTLD_NOW | RTLD_LOCAL);
    } else if (lib.p) {
        int fd = memfd_create("libonnxruntime.so", MFD_CLOEXEC);
        if (fd < 0) err(1, "memfd_create");
        for (size_t off = 0; off < lib.n;) {
            ssize_t w = write(fd, lib.p + off, lib.n - off);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) err(1, "copying ONNX Runtime to memory");
            off += (size_t)w;
        }
        char path[64];
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        close(fd);
    } else {
        h = dlopen("libonnxruntime.so.1", RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) errx(1, "cannot load ONNX Runtime: %s", dlerror());
    const OrtApiBase *(*get)(void) = (const OrtApiBase *(*)(void))dlsym(h, "OrtGetApiBase");
    if (!get) errx(1, "ONNX Runtime has no OrtGetApiBase: %s", dlerror());
    return base = get();
}
