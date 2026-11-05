/* diary.h - encrypted local recordings (see diary.c). The key is compiled in (localkey.h). */
#pragma once
#include <stddef.h>

typedef struct DiaryW DiaryW;

/* Creates FILE (it must not exist, mode 0600) and writes the header. Exits with a message on failure. */
DiaryW *diary_create(const char *path);
/* Appends 16 kHz mono 16-bit PCM; every full second of audio is encrypted and flushed to disk. */
void diary_write(DiaryW *w, const void *pcm, size_t n);
/* Writes the final chunk and closes the file. */
void diary_finish(DiaryW *w);

/* Decrypts FILE to out_fd. Returns 0 when complete, 3 when cut short (power cut: everything up to the last whole
 * chunk was written), 4 when a chunk failed authentication (damaged or tampered file), 1 when it is no diary file. */
int diary_cat(const char *path, int out_fd);

int diary_main(int argc, char **argv);
