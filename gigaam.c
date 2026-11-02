/*
 * gigaam.c - Russian speech recognition with GigaAM-v3 (RNN-T, int8) in plain C.
 *
 * Runs the ONNX models through ONNX Runtime's C API. No Python at runtime.
 * Pipeline (same as the onnx-asr Python package):
 *   16 kHz mono PCM -> gigaam_v3.onnx (log-mel features) -> encoder
 *   -> greedy RNN-T loop (decoder LSTM + joint network) -> text
 *
 * Long audio is cut into chunks at quiet points to bound memory use.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "onnxruntime_c_api.h"

#define SAMPLE_RATE 16000
#define PRED_DIM 320        /* decoder LSTM hidden size */
#define MAX_TOK_PER_STEP 3  /* max tokens emitted per encoder frame */

static const OrtApi *ort;

static void die_ort(OrtStatus *st, const char *what, int line) {
    fprintf(stderr, "onnxruntime error at line %d (%s): %s\n", line, what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    exit(1);
}
#define CK(x) do { OrtStatus *s_ = (x); if (s_) die_ort(s_, #x, __LINE__); } while (0)

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---------------------------------------------------------------- WAV -- */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Reads a 16-bit PCM, 16 kHz, mono WAV into float samples in [-1, 1). */
static float *read_wav(const char *path, size_t *n_samples) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a RIFF/WAVE file\n", path); exit(1);
    }
    int have_fmt = 0;
    uint16_t tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t sz = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) { fprintf(stderr, "bad fmt chunk\n"); exit(1); }
            tag = le16(fm); ch = le16(fm + 2); rate = le32(fm + 4); bits = le16(fm + 14);
            have_fmt = 1;
            fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt) { fprintf(stderr, "data chunk before fmt chunk\n"); exit(1); }
            if ((tag != 1 && tag != 0xFFFE) || bits != 16 || ch != 1 || rate != SAMPLE_RATE) {
                fprintf(stderr, "%s: need 16-bit PCM, mono, %d Hz (got tag=%u, %u-bit, %u ch, %u Hz).\n"
                                "Convert with: ffmpeg -i in.m4a -ar 16000 -ac 1 -c:a pcm_s16le out.wav\n",
                        path, SAMPLE_RATE, tag, bits, ch, rate);
                exit(1);
            }
            long pos = ftell(f);
            fseek(f, 0, SEEK_END);
            long avail = ftell(f) - pos;
            fseek(f, pos, SEEK_SET);
            size_t bytes = (sz == 0xFFFFFFFFu || (long)sz > avail) ? (size_t)avail : sz;
            size_t n = bytes / 2;
            int16_t *raw = xmalloc(n * 2);
            if (fread(raw, 2, n, f) != n) { fprintf(stderr, "short read\n"); exit(1); }
            fclose(f);
            float *out = xmalloc(n * sizeof(float));
            for (size_t i = 0; i < n; i++) {  /* assumes little-endian host (x86, ARM) */
                out[i] = raw[i] / 32768.0f;
            }
            free(raw);
            *n_samples = n;
            return out;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    fprintf(stderr, "%s: no data chunk found\n", path);
    exit(1);
}

/* -------------------------------------------------------------- engine -- */

typedef struct {
    OrtEnv *env;
    OrtMemoryInfo *mem;
    OrtSession *pre, *enc, *dec, *joi;
    char **vocab;     /* token strings, U+2581 already replaced by ' ' */
    int vocab_size;
    int blank;
} Engine;

static OrtSession *open_session(Engine *E, const char *dir, const char *file, int threads) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    if (access(path, R_OK)) { fprintf(stderr, "missing model file: %s\n(run ./fetch.sh)\n", path); exit(1); }
    OrtSessionOptions *so;
    CK(ort->CreateSessionOptions(&so));
    CK(ort->SetIntraOpNumThreads(so, threads));
    CK(ort->SetInterOpNumThreads(so, 1));
    CK(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL));
    OrtSession *s;
    CK(ort->CreateSession(E->env, path, so, &s));
    ort->ReleaseSessionOptions(so);
    return s;
}

static void load_vocab(Engine *E, const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/v3_e2e_rnnt_vocab.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    int cap = 2048;
    E->vocab = xmalloc(cap * sizeof(char *));
    for (int i = 0; i < cap; i++) E->vocab[i] = NULL;
    char line[512];
    int maxid = -1;
    E->blank = -1;
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char *sp = strrchr(line, ' ');
        if (!sp) continue;
        int id = atoi(sp + 1);
        *sp = 0;
        if (id < 0 || id >= cap) continue;
        /* replace U+2581 (E2 96 81) with a plain space */
        char *out = xmalloc(len + 1), *o = out;
        for (const char *p = line; *p;) {
            if ((uint8_t)p[0] == 0xE2 && (uint8_t)p[1] == 0x96 && (uint8_t)p[2] == 0x81) { *o++ = ' '; p += 3; }
            else *o++ = *p++;
        }
        *o = 0;
        E->vocab[id] = out;
        if (id > maxid) maxid = id;
        if (!strcmp(out, "<blk>")) E->blank = id;
    }
    fclose(f);
    E->vocab_size = maxid + 1;
    if (E->blank < 0) { fprintf(stderr, "no <blk> token in vocab\n"); exit(1); }
}

static void engine_init(Engine *E, const char *dir, int threads) {
    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) { fprintf(stderr, "onnxruntime library is older than the headers\n"); exit(1); }
    CK(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "gigaam", &E->env));
    CK(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &E->mem));
    load_vocab(E, dir);
    E->pre = open_session(E, dir, "gigaam_v3.onnx", threads);
    E->enc = open_session(E, dir, "v3_e2e_rnnt_encoder.int8.onnx", threads);
    E->dec = open_session(E, dir, "v3_e2e_rnnt_decoder.int8.onnx", 1);
    E->joi = open_session(E, dir, "v3_e2e_rnnt_joint.int8.onnx", 1);
}

static OrtValue *tensor_over(Engine *E, void *data, size_t bytes, const int64_t *shape, size_t nd,
                             ONNXTensorElementDataType type) {
    OrtValue *v;
    CK(ort->CreateTensorWithDataAsOrtValue(E->mem, data, bytes, shape, nd, type, &v));
    return v;
}

/* ------------------------------------------------------- text cleanup -- */

/* Rough "is a word character" test for a Unicode code point (letters, digits, '_'). */
static int is_word(uint32_t cp) {
    if (cp < 128) return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    if (cp < 0xC0) return cp == 0xAA || cp == 0xB5 || cp == 0xBA;      /* Latin-1 punctuation/symbols */
    if (cp == 0xD7 || cp == 0xF7) return 0;
    if (cp >= 0x2000 && cp <= 0x2BFF) return 0;                         /* general punctuation, symbols */
    if (cp >= 0x3000 && cp <= 0x303F) return 0;
    return 1;
}

static uint32_t next_cp(const char *s, int *len) {
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) { *len = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) { *len = 2; return (p[0] & 0x1F) << 6 | (p[1] & 0x3F); }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) { *len = 3; return (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F); }
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { *len = 4; return (p[0] & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F); }
    *len = 1; return p[0];
}

/* Same rule as the Python package: keep a space only if a word character follows it. */
static void tidy_spaces(char *s) {
    char *o = s;
    for (const char *p = s; *p;) {
        if (*p == ' ') {
            int l;
            uint32_t nxt = p[1] ? next_cp(p + 1, &l) : 0;
            if (p != s && p[1] && is_word(nxt)) *o++ = ' ';
            p++;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
}

/* ------------------------------------------------------ recognition -- */

static int argmax(const float *x, int n) {
    int best = 0;
    for (int i = 1; i < n; i++) if (x[i] > x[best]) best = i;
    return best;
}

/* Transcribes one chunk of 16 kHz mono float PCM. Returns a malloc'd UTF-8 string. */
static char *transcribe(Engine *E, float *pcm, size_t n) {
    /* 1. features */
    int64_t wshape[2] = {1, (int64_t)n}, one[1] = {1}, len = (int64_t)n;
    OrtValue *wav_v = tensor_over(E, pcm, n * sizeof(float), wshape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *len_v = tensor_over(E, &len, sizeof len, one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    const char *pre_in[] = {"waveforms", "waveforms_lens"}, *pre_out[] = {"features", "features_lens"};
    const OrtValue *pre_args[] = {wav_v, len_v};
    OrtValue *feats[2] = {NULL, NULL};
    CK(ort->Run(E->pre, NULL, pre_in, pre_args, 2, pre_out, 2, feats));

    /* 2. encoder */
    const char *enc_in[] = {"audio_signal", "length"}, *enc_out[] = {"encoded", "encoded_len"};
    const OrtValue *enc_args[] = {feats[0], feats[1]};
    OrtValue *eo[2] = {NULL, NULL};
    CK(ort->Run(E->enc, NULL, enc_in, enc_args, 2, enc_out, 2, eo));

    OrtTensorTypeAndShapeInfo *info;
    CK(ort->GetTensorTypeAndShape(eo[0], &info));
    size_t nd;
    CK(ort->GetDimensionsCount(info, &nd));
    int64_t dims[4] = {0};
    if (nd != 3) { fprintf(stderr, "unexpected encoder output rank %zu\n", nd); exit(1); }
    CK(ort->GetDimensions(info, dims, nd));
    ort->ReleaseTensorTypeAndShapeInfo(info);
    int64_t D = dims[1], T = dims[2];   /* encoded is [1, D, T] */
    float *enc;
    int32_t *enc_len_p;
    CK(ort->GetTensorMutableData(eo[0], (void **)&enc));
    CK(ort->GetTensorMutableData(eo[1], (void **)&enc_len_p));
    int64_t Tuse = *enc_len_p < T ? *enc_len_p : T;
    if (D != 768) { fprintf(stderr, "unexpected encoder width %lld\n", (long long)D); exit(1); }

    /* 3. greedy RNN-T decoding with fixed buffers bound to the sessions */
    const int V = E->vocab_size;
    float *enc_vec = xmalloc(D * sizeof(float));
    float *joint_out = xmalloc(V * sizeof(float));
    float dec_buf[PRED_DIM], h_in[PRED_DIM] = {0}, c_in[PRED_DIM] = {0}, h_out[PRED_DIM], c_out[PRED_DIM];
    int64_t x_buf[1] = {E->blank};

    int64_t s_x[2] = {1, 1}, s_st[3] = {1, 1, PRED_DIM}, s_dec_j[3] = {1, PRED_DIM, 1}, s_enc[3] = {1, D, 1},
            s_joint[4] = {1, 1, 1, V};
    OrtValue *x_v = tensor_over(E, x_buf, sizeof x_buf, s_x, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    OrtValue *h_in_v = tensor_over(E, h_in, sizeof h_in, s_st, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *c_in_v = tensor_over(E, c_in, sizeof c_in, s_st, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *dec_v = tensor_over(E, dec_buf, sizeof dec_buf, s_st, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *h_out_v = tensor_over(E, h_out, sizeof h_out, s_st, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *c_out_v = tensor_over(E, c_out, sizeof c_out, s_st, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *dec_j_v = tensor_over(E, dec_buf, sizeof dec_buf, s_dec_j, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *enc_v = tensor_over(E, enc_vec, D * sizeof(float), s_enc, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *joint_v = tensor_over(E, joint_out, V * sizeof(float), s_joint, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);

    const char *dec_in[] = {"x", "h.1", "c.1"}, *dec_outn[] = {"dec", "h", "c"};
    const OrtValue *dec_args[] = {x_v, h_in_v, c_in_v};
    OrtValue *dec_outs[] = {dec_v, h_out_v, c_out_v};
    const char *joi_in[] = {"enc", "dec"}, *joi_outn[] = {"joint"};
    const OrtValue *joi_args[] = {enc_v, dec_j_v};
    OrtValue *joi_outs[] = {joint_v};

    int *tokens = xmalloc((Tuse * MAX_TOK_PER_STEP + 1) * sizeof(int));
    size_t ntok = 0;
    int have_dec = 0, emitted = 0;
    for (int64_t t = 0; t < Tuse;) {
        if (!have_dec) {
            x_buf[0] = ntok ? tokens[ntok - 1] : E->blank;
            CK(ort->Run(E->dec, NULL, dec_in, dec_args, 3, dec_outn, 3, dec_outs));
            have_dec = 1;
        }
        for (int64_t d = 0; d < D; d++) enc_vec[d] = enc[d * T + t];
        CK(ort->Run(E->joi, NULL, joi_in, joi_args, 2, joi_outn, 1, joi_outs));
        int tok = argmax(joint_out, V);
        if (tok != E->blank) {
            tokens[ntok++] = tok;
            memcpy(h_in, h_out, sizeof h_in);
            memcpy(c_in, c_out, sizeof c_in);
            have_dec = 0;
            emitted++;
        }
        if (tok == E->blank || emitted == MAX_TOK_PER_STEP) { t++; emitted = 0; }
    }

    /* 4. tokens -> text */
    size_t total = 1;
    for (size_t i = 0; i < ntok; i++) if (tokens[i] > 0 && E->vocab[tokens[i]]) total += strlen(E->vocab[tokens[i]]);
    char *text = xmalloc(total), *o = text;
    for (size_t i = 0; i < ntok; i++) {      /* token 0 is <unk>: skipped */
        if (tokens[i] > 0 && E->vocab[tokens[i]]) { size_t l = strlen(E->vocab[tokens[i]]); memcpy(o, E->vocab[tokens[i]], l); o += l; }
    }
    *o = 0;
    tidy_spaces(text);

    free(tokens); free(enc_vec); free(joint_out);
    ort->ReleaseValue(x_v); ort->ReleaseValue(h_in_v); ort->ReleaseValue(c_in_v); ort->ReleaseValue(dec_v);
    ort->ReleaseValue(h_out_v); ort->ReleaseValue(c_out_v); ort->ReleaseValue(dec_j_v); ort->ReleaseValue(enc_v);
    ort->ReleaseValue(joint_v);
    ort->ReleaseValue(eo[0]); ort->ReleaseValue(eo[1]);
    ort->ReleaseValue(feats[0]); ort->ReleaseValue(feats[1]);
    ort->ReleaseValue(wav_v); ort->ReleaseValue(len_v);
    return text;
}

/* Picks a cut point in the last `search` samples before `end`: the quietest 50 ms window. */
static size_t find_cut(const float *x, size_t start, size_t end, size_t search) {
    size_t win = SAMPLE_RATE / 20, lo = end > search ? end - search : start;
    if (lo < start + SAMPLE_RATE / 2) lo = start + SAMPLE_RATE / 2;
    size_t best = end;
    double best_e = 1e300;
    for (size_t p = lo; p + win <= end; p += win / 2) {
        double e = 0;
        for (size_t i = 0; i < win; i++) e += (double)x[p + i] * x[p + i];
        if (e < best_e) { best_e = e; best = p + win / 2; }
    }
    return best;
}

static void usage(const char *a0) {
    fprintf(stderr,
            "usage: %s [-m model_dir] [-t threads] [-c chunk_seconds] [-v] audio.wav\n"
            "  -m DIR   directory with the model files (default: ./models)\n"
            "  -t N     CPU threads (default: all cores)\n"
            "  -c SEC   max chunk length; longer audio is cut at quiet points (default 20, 0 = no chunking)\n"
            "  -v       print timing and peak memory to stderr\n"
            "audio.wav must be 16-bit PCM, mono, 16 kHz.\n", a0);
}

int main(int argc, char **argv) {
    const char *model_dir = "models", *wav_path = NULL;
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN), verbose = 0;
    double chunk_sec = 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) model_dir = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) chunk_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] == '-') { usage(argv[0]); return 2; }
        else wav_path = argv[i];
    }
    if (!wav_path) { usage(argv[0]); return 2; }
    if (threads < 1) threads = 1;

    size_t n;
    float *pcm = read_wav(wav_path, &n);
    double t0 = now();
    Engine E = {0};
    engine_init(&E, model_dir, threads);
    double t_load = now() - t0;

    size_t chunk = chunk_sec > 0 ? (size_t)(chunk_sec * SAMPLE_RATE) : n;
    size_t start = 0;
    int nseg = 0;
    t0 = now();
    while (start < n) {
        size_t end = start + chunk;
        if (end >= n || n - end < SAMPLE_RATE) end = n;   /* don't leave a tiny tail */
        else end = find_cut(pcm, start, end, (size_t)3 * SAMPLE_RATE);
        if (end - start >= 400) {                          /* skip < 25 ms of audio */
            char *txt = transcribe(&E, pcm + start, end - start);
            if (*txt) printf("%s%s", nseg ? " " : "", txt);
            free(txt);
            nseg++;
        }
        start = end;
    }
    printf("\n");
    if (verbose) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        double dt = now() - t0;
        fprintf(stderr, "audio %.1f s, %d chunk(s), load %.1f s, decode %.1f s (%.2fx real time), peak RSS %ld MB, %d thread(s)\n",
                (double)n / SAMPLE_RATE, nseg, t_load, dt, dt / ((double)n / SAMPLE_RATE), ru.ru_maxrss / 1024, threads);
    }
    return 0;
}
