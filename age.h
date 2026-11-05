/* age.h - writes files in the age encryption format (https://age-encryption.org/v1) for one X25519 recipient.
 * They open with the standard `age -d -i key.txt` and nothing on this device can decrypt them: only the owner of the
 * matching private key can. */
#pragma once
#include <stddef.h>

typedef struct AgeW AgeW;

/* Starts an age stream on fd for the recipient "age1..." (a public key made with `age-keygen`).
 * Returns NULL, with errno set to EINVAL, if the recipient is not a valid key. Writes the header to fd. */
AgeW *age_open(int fd, const char *recipient);
/* Appends plaintext. Returns 0, or -1 with errno set when writing to fd fails. */
int age_write(AgeW *w, const void *data, size_t n);
/* Writes the final chunk and frees w (fd stays open). Returns 0, or -1 on a write error. */
int age_close(AgeW *w);

/* `sirius age RECIPIENT < plain > encrypted`, for checking a key and the format. */
int age_main(int argc, char **argv);
