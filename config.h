/* config.h - settings of the diary, read from /etc/sirius.conf (KEY=VALUE lines, # comments). */
#pragma once

typedef struct {
    char data_dir[256];        /* where recordings are kept                       DATA_DIR   /var/lib/sirius */
    char recipient[128];       /* age public key the entries are encrypted to     AGE_RECIPIENT  age1...    */
    char bucket[128];          /*                                                  S3_BUCKET   */
    char region[64];           /*                                                  S3_REGION   */
    char access_key[128];      /*                                                  S3_ACCESS_KEY */
    char secret_key[256];      /*                                                  S3_SECRET_KEY */
    char prefix[128];          /* object key prefix                               S3_PREFIX   entries/ */
    char endpoint[256];        /* S3-compatible server, empty for Amazon          S3_ENDPOINT */
    char ca_file[256];         /* extra CA certificates for that server           S3_CA_FILE  */
    char mic[64];              /* ALSA device, empty = first USB/capture card     MIC  */
    char evdev[256];           /* /dev/input/... of a USB button or keyboard      EVDEV */
    char models_dir[256];      /* speech models on disk instead of built in       MODELS_DIR (development) */
    double gate;               /* speech level for finding utterances             GATE  0.005 */
    int flip;                  /* 1 = turn the display picture by 180 degrees     FLIP  */
    int threads;               /* CPU threads for transcription, 0 = auto         THREADS */
} Config;

/* Fills c with the defaults, then the settings in `path` (NULL = $SIRIUS_CONF or /etc/sirius.conf). A missing file is
 * not an error. Returns 0, or -1 with a message in err. */
int config_load(Config *c, const char *path, char *err, size_t n);
/* Writes the settings to `path` (mode 0600, replaced atomically). Returns 0, or -1 with errno set. */
int config_save(const Config *c, const char *path);
/* Returns an error text if something needed for uploading is missing, else NULL. */
const char *config_missing(const Config *c);
