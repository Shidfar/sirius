/* entry.h - the diary's store: recordings waiting to be transcribed, and encrypted entries waiting to be uploaded.
 *
 *   <data_dir>/entries/<id>/audio.enc            the recording, encrypted with the key built into the program
 *   <data_dir>/entries/<id>/transcript.txt.age   after transcription: encrypted to the owner's public key
 *   <data_dir>/entries/<id>/audio.opus.age       likewise, the recording compressed with Opus
 * Once the two .age files exist audio.enc is deleted; after a confirmed upload each .age file is deleted.
 */
#pragma once
#include <stddef.h>

#include "config.h"

typedef struct { int recording, to_transcribe, to_upload, stuck; } StoreCounts;

/* Makes the directory for a new entry named after the current time; id and dir get its name and path. */
int entry_create(const Config *c, char *id, size_t idn, char *dir, size_t dirn);
/* Counts what is in the store. */
void store_counts(const Config *c, StoreCounts *n);
/* Transcribes and encrypts every finished recording (the speech model is loaded only if there is one). Returns the
 * number of entries done, or -1 on a setup error (message in err). */
int store_transcribe(const Config *c, char *err, size_t errn);
/* Uploads every .age file. Returns S3_OK, S3_RETRY (offline: stopped early) or S3_FAILED (refused: stopped early);
 * `sent` gets the number of files uploaded, err the reason for a failure. */
int store_upload(const Config *c, int *sent, char *err, size_t errn);

/* `sirius sync` (transcribe + upload now) and `sirius status` (what is waiting). */
int sync_main(int argc, char **argv);
int status_main(int argc, char **argv);
