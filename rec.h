/* rec.h - recording from a microphone straight into an encrypted diary file. */
#pragma once
#include <signal.h>
#include <stddef.h>

/* The ALSA device of the first sound card that can record ("plughw:1,0"), or NULL if there is none. */
const char *rec_find_mic(char *buf, size_t n);

/* Records 16 kHz mono audio from the ALSA device `pcm` (NULL: rec_find_mic) into the new file `path`, until *stop
 * becomes non-zero. Returns 0, or -1 with a message in err. If the microphone fails half-way, the file is still
 * finished properly and the message says so (return -2). */
int rec_run(const char *pcm, const char *path, volatile sig_atomic_t *stop, char *err, size_t n);

int rec_main(int argc, char **argv);
