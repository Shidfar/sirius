/*
 * oggopus.c - Opus-in-Ogg writer (RFC 7845) for 16 kHz mono speech. The recording is kept in this form (about
 * 3 KB per second at 24 kbps) next to the transcript.
 */
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ogg/ogg.h>
#include <opus/opus.h>
#include <sodium.h>

#include "oggopus.h"

#define RATE 16000
#define FRAME 320                                /* 20 ms */
#define MAX_PACKET 1500

struct OggOpus {
    OpusEncoder *enc;
    ogg_stream_state os;
    OggSink sink;
    void *ctx;
    int16_t frame[FRAME];
    size_t fill;
    uint64_t samples;                            /* input samples so far */
    ogg_int64_t granule;                         /* position at 48 kHz, as the format wants it */
    int preskip, err;
    ogg_packet pending;                          /* one packet is held back so the last one can be marked */
    unsigned char pending_data[MAX_PACKET];
    int have_pending;
};

static void put_le(unsigned char *p, uint32_t v, int bytes) { for (int i = 0; i < bytes; i++) p[i] = (unsigned char)(v >> (8 * i)); }

static void emit_pages(OggOpus *o, int force) {
    ogg_page pg;
    while ((force ? ogg_stream_flush(&o->os, &pg) : ogg_stream_pageout(&o->os, &pg)) > 0) {
        if (!o->err) o->err = o->sink(o->ctx, pg.header, (size_t)pg.header_len);
        if (!o->err) o->err = o->sink(o->ctx, pg.body, (size_t)pg.body_len);
    }
}

OggOpus *oggopus_open(OggSink sink, void *ctx, int bitrate) {
    OggOpus *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    int e;
    o->enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &e);
    if (e != OPUS_OK || !o->enc) { free(o); return NULL; }
    opus_encoder_ctl(o->enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(o->enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(o->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(o->enc, OPUS_SET_COMPLEXITY(8));
    opus_int32 look = 0;
    opus_encoder_ctl(o->enc, OPUS_GET_LOOKAHEAD(&look));
    o->preskip = look * (48000 / RATE);          /* the lookahead is counted in input samples; the header wants 48 kHz */
    o->sink = sink;
    o->ctx = ctx;
    ogg_stream_init(&o->os, (int)randombytes_uniform(0x7fffffff));

    unsigned char head[19] = "OpusHead";
    head[8] = 1;                                 /* version */
    head[9] = 1;                                 /* channels */
    put_le(head + 10, (uint32_t)o->preskip, 2);
    put_le(head + 12, RATE, 4);                  /* original input rate, informational */
    put_le(head + 16, 0, 2);                     /* output gain */
    head[18] = 0;                                /* channel mapping family 0 */
    static const char vendor[] = "sirius";
    unsigned char tags[8 + 4 + sizeof vendor - 1 + 4] = "OpusTags";
    put_le(tags + 8, sizeof vendor - 1, 4);
    memcpy(tags + 12, vendor, sizeof vendor - 1);
    put_le(tags + 12 + sizeof vendor - 1, 0, 4); /* no comments */
    ogg_packet p = {.packet = head, .bytes = sizeof head, .b_o_s = 1, .e_o_s = 0, .granulepos = 0, .packetno = 0};
    ogg_stream_packetin(&o->os, &p);
    emit_pages(o, 1);                            /* the headers each get their own page */
    p = (ogg_packet){.packet = tags, .bytes = sizeof tags, .b_o_s = 0, .e_o_s = 0, .granulepos = 0, .packetno = 1};
    ogg_stream_packetin(&o->os, &p);
    emit_pages(o, 1);
    if (o->err) { opus_encoder_destroy(o->enc); ogg_stream_clear(&o->os); free(o); return NULL; }
    return o;
}

/* Hands the held-back packet to Ogg; `last` marks the end of the stream and trims the padding of the final frame. */
static void release_pending(OggOpus *o, int last, ogg_int64_t end_granule) {
    if (!o->have_pending) return;
    o->pending.packet = o->pending_data;
    o->pending.e_o_s = last;
    if (last) o->pending.granulepos = end_granule;
    ogg_stream_packetin(&o->os, &o->pending);
    o->have_pending = 0;
}

static void encode_frame(OggOpus *o) {
    unsigned char out[MAX_PACKET];
    int n = opus_encode(o->enc, o->frame, FRAME, out, sizeof out);
    if (n < 0) { o->err = EIO; return; }
    release_pending(o, 0, 0);
    o->granule += FRAME * (48000 / RATE);
    memcpy(o->pending_data, out, (size_t)n);
    o->pending = (ogg_packet){.bytes = n, .b_o_s = 0, .e_o_s = 0, .granulepos = o->granule,
                              .packetno = 2 + (o->granule / (FRAME * (48000 / RATE)) - 1)};
    o->have_pending = 1;
    emit_pages(o, 0);
}

int oggopus_write(OggOpus *o, const int16_t *pcm, size_t n) {
    o->samples += n;
    while (n && !o->err) {
        size_t take = FRAME - o->fill < n ? FRAME - o->fill : n;
        memcpy(o->frame + o->fill, pcm, take * sizeof *pcm);
        o->fill += take; pcm += take; n -= take;
        if (o->fill == FRAME) { encode_frame(o); o->fill = 0; }
    }
    return o->err;
}

int oggopus_close(OggOpus *o) {
    if (o->fill) {                               /* pad the last frame with silence */
        memset(o->frame + o->fill, 0, (FRAME - o->fill) * sizeof *o->frame);
        encode_frame(o);
    }
    /* The decoder's output starts `preskip` late, so silence is encoded until the whole input has come out
     * (this also gives an empty input a valid one-frame stream). */
    while (o->granule < (ogg_int64_t)(o->samples * (48000 / RATE)) + o->preskip) {
        memset(o->frame, 0, sizeof o->frame);
        encode_frame(o);
    }
    release_pending(o, 1, (ogg_int64_t)(o->samples * (48000 / RATE)) + o->preskip);
    emit_pages(o, 1);
    int rc = o->err;
    opus_encoder_destroy(o->enc);
    ogg_stream_clear(&o->os);
    free(o);
    return rc;
}

static int to_fd(void *ctx, const void *data, size_t n) {
    int fd = *(int *)ctx;
    const unsigned char *p = data;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) return errno;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int oggopus_main(int argc, char **argv) {
    int kbps = 24;
    if (argc == 3 && !strcmp(argv[1], "-b")) kbps = atoi(argv[2]);
    else if (argc != 1) { fprintf(stderr, "usage: %s [-b kbps] < raw_pcm > file.opus\n", argv[0]); return 2; }
    int out = STDOUT_FILENO;
    OggOpus *o = oggopus_open(to_fd, &out, kbps * 1000);
    if (!o) errx(1, "cannot start the Opus encoder");
    int16_t buf[4096];
    size_t have = 0;
    ssize_t r;
    while ((r = read(STDIN_FILENO, (char *)buf + have, sizeof buf - have)) > 0) {
        have += (size_t)r;
        size_t whole = have / 2;
        if (oggopus_write(o, buf, whole)) errx(1, "write failed");
        have -= whole * 2;
        if (have) memmove(buf, (char *)buf + whole * 2, have);
    }
    if (oggopus_close(o)) errx(1, "write failed");
    return 0;
}
