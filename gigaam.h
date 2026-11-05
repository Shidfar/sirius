/* gigaam.h - speech recognition (GigaAM-v3 RNN-T on ONNX Runtime). See gigaam.c. */
#pragma once
#include <stdio.h>

typedef struct Engine Engine;

/* Loads the models: the ones built into the binary, or the files in model_dir when it is not NULL.
 * threads <= 0 means all cores. Exits with a message on failure. */
Engine *gigaam_open(const char *model_dir, int threads);
void gigaam_close(Engine *E);

/* Reads raw 16 kHz mono 16-bit PCM from fd until it ends, finds utterances with a simple energy gate and writes one
 * line of text per utterance to out. With stamps each line starts with "[h:mm:ss] ", the time of the utterance's start. */
void gigaam_stream(Engine *E, int fd, FILE *out, int stamps, double gate, double chunk_sec, int verbose);

int gigaam_main(int argc, char **argv);
