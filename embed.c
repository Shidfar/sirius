/*
 * embed.c - the files that make the binary self-contained, pulled in with the assembler's .incbin.
 * Build with -DEMBED and the EMBED_* paths from the Makefile; without -DEMBED nothing is built in
 * (development builds read the models from a directory and load libonnxruntime.so from the system).
 */
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "embed.h"

#ifdef EMBED
#define INCBIN(id, path)                                                                          \
    __asm__(".section .rodata\n.balign 4096\n.global " #id "\n" #id ":\n.incbin \"" path "\"\n" \
            ".global " #id "_end\n" #id "_end:\n.byte 0\n.previous\n");                           \
    extern const unsigned char id[], id##_end[];

INCBIN(blob_ort, EMBED_ORT)
INCBIN(blob_pre, EMBED_MODELS "/gigaam_v3.onnx")
INCBIN(blob_enc, EMBED_MODELS "/v3_e2e_rnnt_encoder.int8.onnx")
INCBIN(blob_dec, EMBED_MODELS "/v3_e2e_rnnt_decoder.int8.onnx")
INCBIN(blob_joi, EMBED_MODELS "/v3_e2e_rnnt_joint.int8.onnx")
INCBIN(blob_voc, EMBED_MODELS "/v3_e2e_rnnt_vocab.txt")
INCBIN(blob_ca, EMBED_CERTS "/amazon-roots.pem")

#define ENTRY(name, id) {name, id, id##_end}
static const struct { const char *name; const unsigned char *start, *end; } table[] = {
    ENTRY("libonnxruntime.so", blob_ort),
    ENTRY("gigaam_v3.onnx", blob_pre),
    ENTRY("v3_e2e_rnnt_encoder.int8.onnx", blob_enc),
    ENTRY("v3_e2e_rnnt_decoder.int8.onnx", blob_dec),
    ENTRY("v3_e2e_rnnt_joint.int8.onnx", blob_joi),
    ENTRY("v3_e2e_rnnt_vocab.txt", blob_voc),
    ENTRY("amazon-roots.pem", blob_ca),
};
#endif

Blob embedded(const char *name) {
#ifdef EMBED
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(name, table[i].name)) return (Blob){table[i].start, (size_t)(table[i].end - table[i].start)};
#else
    (void)name;
#endif
    return (Blob){0};
}

/* Tells the kernel the pages of a read-only mapped blob are not needed now (they are read again from the file if
 * ever touched). After ONNX Runtime has parsed a model, this takes the model out of the process's resident memory. */
void blob_release(Blob b) {
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE), lo = ((uintptr_t)b.p + page - 1) & ~(page - 1),
              hi = ((uintptr_t)b.p + b.n) & ~(page - 1);
    if (b.p && hi > lo) madvise((void *)lo, hi - lo, MADV_DONTNEED);
}
