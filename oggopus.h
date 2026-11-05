/* oggopus.h - compresses 16 kHz mono 16-bit audio into an Ogg Opus stream (libopus + libogg). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct OggOpus OggOpus;
/* Receives the bytes of the Ogg stream; returns 0 on success. */
typedef int (*OggSink)(void *ctx, const void *data, size_t n);

/* bitrate in bits per second (24000 is clear for speech). Returns NULL if the encoder can't be created. */
OggOpus *oggopus_open(OggSink sink, void *ctx, int bitrate);
/* Returns 0, or the sink's error. */
int oggopus_write(OggOpus *o, const int16_t *pcm, size_t samples);
/* Flushes the last frame and the end-of-stream page, frees o. Returns 0, or the sink's error. */
int oggopus_close(OggOpus *o);

/* `sirius opus [-b kbps] < raw_pcm > file.opus` */
int oggopus_main(int argc, char **argv);
