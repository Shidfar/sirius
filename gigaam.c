/*
 * gigaam.c - Russian speech recognition with GigaAM-v3 (RNN-T, int8) in plain C.
 *
 * Runs the ONNX models through ONNX Runtime's C API. No Python at runtime.
 * Pipeline (same as the onnx-asr Python package):
 *   16 kHz mono PCM -> gigaam_v3.onnx (log-mel features) -> encoder
 *   -> greedy RNN-T loop (decoder LSTM + joint network) -> text
 *
 * Long audio is cut into chunks at quiet points to bound memory use.
 *
 * Two ways to use it:
 *   gigaam audio.wav          transcribe a WAV file and exit
 *   gigaam -s < raw_pcm       stream mode: read raw 16 kHz mono 16-bit PCM from stdin (for example from
 *                             arecord), load the model once, print one line of text per utterance
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "onnxruntime_c_api.h"

#define SAMPLE_RATE 16000
#define ENC_DIM 768         /* encoder output width */
#define PRED_DIM 320        /* decoder LSTM hidden size */
#define MAX_TOK_PER_STEP 3  /* max tokens emitted per encoder frame */
#define MAX_VOCAB 4096
#define ND(a) (sizeof(a) / sizeof *(a))

static const OrtApi *ort;

#define CK(x) do { OrtStatus *s_ = (x); if (s_) errx(1, "onnxruntime error at line %d (%s): %s", \
                                                    __LINE__, #x, ort->GetErrorMessage(s_)); } while (0)
/* Run a session; the input/output counts come from the array sizes. */
#define RUN(sess, in, args, out, vals) CK(ort->Run(sess, NULL, in, args, ND(args), out, ND(out), vals))

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) errx(1, "out of memory (%zu bytes)", n);
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
    if (!f) err(1, "%s", path);
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        errx(1, "%s: not a RIFF/WAVE file", path);
    int have_fmt = 0;
    uint16_t tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t sz = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16];
            if (sz < 16 || fread(fm, 1, 16, f) != 16) errx(1, "%s: bad fmt chunk", path);
            tag = le16(fm); ch = le16(fm + 2); rate = le32(fm + 4); bits = le16(fm + 14);
            have_fmt = 1;
            fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt) errx(1, "%s: data chunk before fmt chunk", path);
            if ((tag != 1 && tag != 0xFFFE) || bits != 16 || ch != 1 || rate != SAMPLE_RATE)
                errx(1, "%s: need 16-bit PCM, mono, %d Hz (got tag=%u, %u-bit, %u ch, %u Hz).\n"
                        "Convert with: ffmpeg -i in.m4a -ar 16000 -ac 1 -c:a pcm_s16le out.wav",
                     path, SAMPLE_RATE, tag, bits, ch, rate);
            long pos = ftell(f);
            fseek(f, 0, SEEK_END);
            long avail = ftell(f) - pos;
            fseek(f, pos, SEEK_SET);
            size_t bytes = (sz == 0xFFFFFFFFu || (long)sz > avail) ? (size_t)avail : sz;
            size_t n = bytes / 2;
            float *out = xmalloc(n * sizeof *out);
            uint8_t blk[8192];                         /* convert in blocks: no second full-size buffer */
            for (size_t i = 0; i < n;) {
                size_t m = n - i < sizeof blk / 2 ? n - i : sizeof blk / 2;
                if (fread(blk, 2, m, f) != m) errx(1, "%s: short read", path);
                for (size_t k = 0; k < m; k++) out[i + k] = (int16_t)le16(blk + 2 * k) / 32768.0f;
                i += m;
            }
            fclose(f);
            *n_samples = n;
            return out;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
        }
    }
    errx(1, "%s: no data chunk found", path);
}

/* -------------------------------------------------------------- engine -- */

typedef struct {
    OrtEnv *env;
    OrtMemoryInfo *mem;
    OrtSession *pre, *enc, *dec, *joi;
    char *vocab[MAX_VOCAB];  /* token strings, U+2581 already replaced by ' '; NULL = never emitted */
    int vocab_size;
    int blank;
} Engine;

/* Joins dir/file and checks that it is readable. */
static void model_path(char *buf, size_t n, const char *dir, const char *file) {
    snprintf(buf, n, "%s/%s", dir, file);
    if (access(buf, R_OK)) errx(1, "missing model file: %s (run ./fetch.sh)", buf);
}

static OrtSession *open_session(Engine *E, const char *dir, const char *file, int threads) {
    char path[4096];
    model_path(path, sizeof path, dir, file);
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
    model_path(path, sizeof path, dir, "v3_e2e_rnnt_vocab.txt");
    FILE *f = fopen(path, "rb");
    if (!f) err(1, "%s", path);
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    E->blank = -1;
    while ((len = getline(&line, &cap, f)) > 0) {          /* lines look like "<token> <id>" */
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char *sp = strrchr(line, ' ');
        if (!sp) continue;
        int id = atoi(sp + 1);
        *sp = 0;
        if (id < 0 || id >= MAX_VOCAB) errx(1, "%s: token id %d out of range (max %d)", path, id, MAX_VOCAB - 1);
        if (!strcmp(line, "<unk>")) continue;               /* never printed */
        char *o = line;                                     /* replace U+2581 (E2 96 81) with ' ', in place */
        for (const char *p = line; *p;) {
            if (!strncmp(p, "\xE2\x96\x81", 3)) { *o++ = ' '; p += 3; }
            else *o++ = *p++;
        }
        *o = 0;
        E->vocab[id] = memcpy(xmalloc(o - line + 1), line, o - line + 1);
        if (id >= E->vocab_size) E->vocab_size = id + 1;
        if (!strcmp(line, "<blk>")) E->blank = id;
    }
    free(line);
    fclose(f);
    if (E->blank < 0) errx(1, "%s: no <blk> token", path);
}

static void engine_init(Engine *E, const char *dir, int threads) {
    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) errx(1, "onnxruntime library is older than the headers");
    CK(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "gigaam", &E->env));
    CK(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &E->mem));
    load_vocab(E, dir);
    E->pre = open_session(E, dir, "gigaam_v3.onnx", threads);
    E->enc = open_session(E, dir, "v3_e2e_rnnt_encoder.int8.onnx", threads);
    E->dec = open_session(E, dir, "v3_e2e_rnnt_decoder.int8.onnx", 1);
    E->joi = open_session(E, dir, "v3_e2e_rnnt_joint.int8.onnx", 1);
}

/* Wraps caller-owned memory in an ORT tensor; the byte size comes from the shape. */
static OrtValue *tensor_over(Engine *E, void *data, size_t elem_size, ONNXTensorElementDataType type,
                             const int64_t *shape, size_t nd) {
    size_t count = 1;
    for (size_t i = 0; i < nd; i++) count *= (size_t)shape[i];
    OrtValue *v;
    CK(ort->CreateTensorWithDataAsOrtValue(E->mem, data, count * elem_size, shape, nd, type, &v));
    return v;
}
#define F32(E, p, shape) tensor_over(E, p, sizeof(float), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, shape, ND(shape))
#define I64(E, p, shape) tensor_over(E, p, sizeof(int64_t), ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, shape, ND(shape))

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

/* Decodes the UTF-8 code point at s (0 at the end of the string). */
static uint32_t next_cp(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0 && p[1]) return (p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) return (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) return (p[0] & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
    return p[0];
}

/* Same rule as the Python package: keep a space only if a word character follows it. */
static void tidy_spaces(char *s) {
    char *o = s;
    for (const char *p = s; *p; p++)
        if (*p != ' ' || (p != s && is_word(next_cp(p + 1)))) *o++ = *p;
    *o = 0;
}

/* ------------------------------------------------------ recognition -- */

static int argmax(const float *x, int n) {
    int best = 0;
    for (int i = 1; i < n; i++) if (x[i] > x[best]) best = i;
    return best;
}

static const char *tok_str(const Engine *E, int id) { return E->vocab[id] ? E->vocab[id] : ""; }

/* Transcribes one chunk of 16 kHz mono float PCM. Returns a malloc'd UTF-8 string. */
static char *transcribe(Engine *E, float *pcm, size_t n) {
    /* 1. features */
    int64_t wshape[2] = {1, (int64_t)n}, one[1] = {1}, len = (int64_t)n;
    OrtValue *wav_v = F32(E, pcm, wshape);
    OrtValue *len_v = I64(E, &len, one);
    const char *pre_in[] = {"waveforms", "waveforms_lens"}, *pre_out[] = {"features", "features_lens"};
    const OrtValue *pre_args[] = {wav_v, len_v};
    OrtValue *feats[2] = {NULL, NULL};
    RUN(E->pre, pre_in, pre_args, pre_out, feats);

    /* 2. encoder */
    const char *enc_in[] = {"audio_signal", "length"}, *enc_out[] = {"encoded", "encoded_len"};
    const OrtValue *enc_args[] = {feats[0], feats[1]};
    OrtValue *eo[2] = {NULL, NULL};
    RUN(E->enc, enc_in, enc_args, enc_out, eo);

    OrtTensorTypeAndShapeInfo *info;
    CK(ort->GetTensorTypeAndShape(eo[0], &info));
    size_t nd;
    int64_t dims[3];                                    /* encoded is [1, ENC_DIM, T] */
    CK(ort->GetDimensionsCount(info, &nd));
    if (nd != 3) errx(1, "unexpected encoder output rank %zu", nd);
    CK(ort->GetDimensions(info, dims, nd));
    ort->ReleaseTensorTypeAndShapeInfo(info);
    if (dims[1] != ENC_DIM) errx(1, "unexpected encoder width %lld", (long long)dims[1]);
    const int64_t T = dims[2];
    float *enc;
    int32_t *enc_len_p;
    CK(ort->GetTensorMutableData(eo[0], (void **)&enc));
    CK(ort->GetTensorMutableData(eo[1], (void **)&enc_len_p));
    const int64_t Tuse = *enc_len_p < T ? *enc_len_p : T;

    /* 3. greedy RNN-T decoding with fixed buffers bound to the sessions */
    const int V = E->vocab_size;
    float enc_vec[ENC_DIM], joint_out[MAX_VOCAB];
    float dec_buf[PRED_DIM], h_in[PRED_DIM] = {0}, c_in[PRED_DIM] = {0}, h_out[PRED_DIM], c_out[PRED_DIM];
    int64_t x_buf[1] = {E->blank};

    int64_t s_x[2] = {1, 1}, s_st[3] = {1, 1, PRED_DIM}, s_dec_j[3] = {1, PRED_DIM, 1}, s_enc[3] = {1, ENC_DIM, 1},
            s_joint[4] = {1, 1, 1, V};
    OrtValue *x_v = I64(E, x_buf, s_x);
    OrtValue *h_in_v = F32(E, h_in, s_st), *c_in_v = F32(E, c_in, s_st);
    OrtValue *h_out_v = F32(E, h_out, s_st), *c_out_v = F32(E, c_out, s_st);
    OrtValue *dec_v = F32(E, dec_buf, s_st);
    OrtValue *dec_j_v = F32(E, dec_buf, s_dec_j);   /* same memory, shaped [1, PRED_DIM, 1] for the joint net */
    OrtValue *enc_v = F32(E, enc_vec, s_enc);
    OrtValue *joint_v = F32(E, joint_out, s_joint);

    const char *dec_in[] = {"x", "h.1", "c.1"}, *dec_outn[] = {"dec", "h", "c"};
    const OrtValue *dec_args[] = {x_v, h_in_v, c_in_v};
    OrtValue *dec_outs[] = {dec_v, h_out_v, c_out_v};
    const char *joi_in[] = {"enc", "dec"}, *joi_outn[] = {"joint"};
    const OrtValue *joi_args[] = {enc_v, dec_j_v};
    OrtValue *joi_outs[] = {joint_v};

    int *tokens = xmalloc((Tuse * MAX_TOK_PER_STEP + 1) * sizeof(int));
    size_t ntok = 0;
    RUN(E->dec, dec_in, dec_args, dec_outn, dec_outs);           /* prime the decoder with <blk> */
    for (int64_t t = 0; t < Tuse; t++) {
        for (int64_t d = 0; d < ENC_DIM; d++) enc_vec[d] = enc[d * T + t];
        for (int e = 0; e < MAX_TOK_PER_STEP; e++) {
            RUN(E->joi, joi_in, joi_args, joi_outn, joi_outs);
            int tok = argmax(joint_out, V);
            if (tok == E->blank) break;                          /* blank: advance to the next frame */
            tokens[ntok++] = tok;
            x_buf[0] = tok;
            memcpy(h_in, h_out, sizeof h_in);
            memcpy(c_in, c_out, sizeof c_in);
            RUN(E->dec, dec_in, dec_args, dec_outn, dec_outs);   /* feed the new token back */
        }
    }

    /* 4. tokens -> text */
    size_t total = 1;
    for (size_t i = 0; i < ntok; i++) total += strlen(tok_str(E, tokens[i]));
    char *text = xmalloc(total), *o = text;
    *o = 0;
    for (size_t i = 0; i < ntok; i++) o = stpcpy(o, tok_str(E, tokens[i]));
    tidy_spaces(text);

    free(tokens);
    OrtValue *owned[] = {x_v, h_in_v, c_in_v, h_out_v, c_out_v, dec_v, dec_j_v, enc_v, joint_v,
                         eo[0], eo[1], feats[0], feats[1], wav_v, len_v};
    for (size_t i = 0; i < ND(owned); i++) ort->ReleaseValue(owned[i]);
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

/* Transcribes pcm[0..n) in pieces of at most `chunk` samples, cut at quiet points, and writes the text to
 * `out`. Returns the number of pieces. */
static int transcribe_long(Engine *E, float *pcm, size_t n, size_t chunk, FILE *out) {
    int nseg = 0, printed = 0;
    for (size_t start = 0; start < n;) {
        size_t end = start + chunk;
        if (end >= n || n - end < SAMPLE_RATE) end = n;   /* don't leave a tiny tail */
        else end = find_cut(pcm, start, end, (size_t)3 * SAMPLE_RATE);
        if (end - start >= 400) {                          /* skip < 25 ms of audio */
            char *txt = transcribe(E, pcm + start, end - start);
            if (*txt) { fprintf(out, "%s%s", printed ? " " : "", txt); printed = 1; }
            free(txt);
            nseg++;
        }
        start = end;
    }
    return nseg;
}

/* ------------------------------------------------------- stream mode -- */

#define FRAME 320               /* 20 ms of audio */
#define PREROLL_FRAMES 15       /* audio kept from before the first loud frame (300 ms) */
#define START_FRAMES 3          /* loud frames in a row that start an utterance */
#define END_FRAMES 35           /* quiet frames in a row that end it (700 ms) */
#define KEEP_QUIET_FRAMES 10    /* quiet audio kept after the last speech (200 ms) */
#define MIN_FRAMES 15           /* shorter utterances are ignored (300 ms) */
#define MAX_UTT_SEC 30          /* longer ones are cut here */
#define NOISE_FRAMES 150        /* the background level is the quietest frame of the last 3 s */

/* Reads one FRAME of samples from stdin. Returns 0 at the end of the input. */
static int read_frame(int16_t *buf) {
    size_t want = sizeof(int16_t) * FRAME, got = 0;
    while (got < want) {
        ssize_t r = read(STDIN_FILENO, (char *)buf + got, want - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

/* Finds utterances in the PCM stream on stdin with a simple energy gate, transcribes each one and prints it
 * as one line. `gate` is the speech level (RMS, 1 = full scale); the gate rises on its own in a noisy room. */
static void stream_loop(Engine *E, size_t chunk, double gate, int verbose) {
#ifdef F_SETPIPE_SZ
    fcntl(STDIN_FILENO, F_SETPIPE_SZ, 1 << 20);   /* the pipe can hold ~30 s of audio while we transcribe */
#endif
    const size_t cap = (size_t)MAX_UTT_SEC * SAMPLE_RATE;
    float *utt = xmalloc(cap * sizeof *utt), frame[FRAME];
    int16_t raw[FRAME];
    size_t n = 0;                                 /* samples in utt */
    double recent[NOISE_FRAMES];                  /* mean-square energy of the last NOISE_FRAMES frames */
    int in_utt = 0, loud_run = 0, quiet_run = 0, next = 0;
    for (int i = 0; i < NOISE_FRAMES; i++) recent[i] = 1e9;

    for (int more = 1; more;) {
        int end_utt = 0;
        more = read_frame(raw);
        if (more) {
            double e = 0, noise;
            for (int i = 0; i < FRAME; i++) { frame[i] = raw[i] / 32768.0f; e += (double)frame[i] * frame[i]; }
            e /= FRAME;
            recent[next] = e;
            next = (next + 1) % NOISE_FRAMES;
            noise = recent[0];
            for (int i = 1; i < NOISE_FRAMES; i++) if (recent[i] < noise) noise = recent[i];
            int loud = e > (gate * gate > 9 * noise ? gate * gate : 9 * noise);   /* gate, or 3x the background */
            if (!in_utt && n >= (size_t)PREROLL_FRAMES * FRAME) {   /* idle: keep just the last 300 ms */
                memmove(utt, utt + FRAME, (n - FRAME) * sizeof *utt);
                n -= FRAME;
            }
            memcpy(utt + n, frame, sizeof frame);
            n += FRAME;
            if (!in_utt) {
                loud_run = loud ? loud_run + 1 : 0;
                if (loud_run >= START_FRAMES) { in_utt = 1; quiet_run = 0; }
            } else {
                quiet_run = loud ? 0 : quiet_run + 1;
                end_utt = quiet_run >= END_FRAMES || n + FRAME > cap;
            }
        } else {
            end_utt = in_utt;                     /* end of input: transcribe what we have */
        }
        if (!end_utt) continue;

        size_t len = n - (quiet_run > KEEP_QUIET_FRAMES ? quiet_run - KEEP_QUIET_FRAMES : 0) * FRAME;
        if (len >= (size_t)MIN_FRAMES * FRAME) {
            char *text = NULL;
            size_t size = 0;
            FILE *ms = open_memstream(&text, &size);
            if (!ms) err(1, "open_memstream");
            double t0 = now();
            int nseg = transcribe_long(E, utt, len, chunk, ms);
            fclose(ms);
            if (size) { fwrite(text, 1, size, stdout); putchar('\n'); fflush(stdout); }
            free(text);
            if (verbose)
                fprintf(stderr, "utterance %.1f s: transcribed in %.1f s (%d chunk(s))\n",
                        (double)len / SAMPLE_RATE, now() - t0, nseg);
        }
        n = 0;
        in_utt = loud_run = quiet_run = 0;
    }
    free(utt);
}

static void usage(const char *a0) {
    fprintf(stderr,
            "usage: %s [-m model_dir] [-t threads] [-c chunk_seconds] [-v] audio.wav\n"
            "       %s -s [-g level] [-m model_dir] [-t threads] [-c chunk_seconds] [-v]\n"
            "  -m DIR   directory with the model files (default: ./models)\n"
            "  -t N     CPU threads (default: all cores)\n"
            "  -c SEC   max chunk length; longer audio is cut at quiet points (default 20, minimum 1, 0 = no chunking)\n"
            "  -s       stream mode: read raw 16 kHz mono 16-bit PCM from stdin, print one line per utterance\n"
            "  -g LEVEL speech level for stream mode, as a fraction of full scale (default 0.005)\n"
            "  -v       print timing and peak memory to stderr\n"
            "audio.wav must be 16-bit PCM, mono, 16 kHz.\n", a0, a0);
}

int main(int argc, char **argv) {
#ifdef __GLIBC__
    /* Pin glibc's mmap threshold. Its dynamic growth (triggered by freeing any large buffer) makes
     * ONNX Runtime's big allocations stay resident: ~100 MB more peak memory, varying with the input. */
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
    const char *model_dir = "models", *wav_path = NULL;
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN), verbose = 0, stream = 0;
    double chunk_sec = 20, gate = 0.005;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) model_dir = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) chunk_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) gate = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s")) stream = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] == '-') { usage(argv[0]); return 2; }
        else wav_path = argv[i];
    }
    if (stream == (wav_path != NULL) || gate <= 0) { usage(argv[0]); return 2; }   /* a file or -s, not both */
    if (threads < 1) threads = 1;

    size_t n = 0;
    float *pcm = stream ? NULL : read_wav(wav_path, &n);
    double t0 = now();
    Engine E = {0};
    engine_init(&E, model_dir, threads);
    double t_load = now() - t0;

    size_t chunk = chunk_sec > 0 ? (size_t)(chunk_sec * SAMPLE_RATE) : (size_t)-1 / 2;
    if (chunk < SAMPLE_RATE) chunk = SAMPLE_RATE;      /* guarantees the chunk loop makes progress */
    if (stream) {
        if (verbose) fprintf(stderr, "model loaded in %.1f s, listening on stdin\n", t_load);
        stream_loop(&E, chunk, gate, verbose);
        return 0;
    }

    t0 = now();
    int nseg = transcribe_long(&E, pcm, n, chunk, stdout);
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
