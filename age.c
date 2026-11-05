/*
 * age.c - encrypts to an age (https://age-encryption.org/v1) X25519 recipient, streaming, with libsodium.
 *
 * File = header + 16-byte nonce + chunks of 64 KiB, each ChaCha20-Poly1305 sealed. The header holds the random
 * 16-byte file key wrapped for the recipient (ephemeral X25519 key, HKDF-SHA256) and an HMAC over the header.
 * The last chunk carries a flag in its nonce, so a cut-off file does not decrypt.
 */
#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

#include "age.h"

#define CHUNK 65536
#define TAG crypto_aead_chacha20poly1305_ietf_ABYTES

struct AgeW {
    int fd;
    unsigned char key[32];               /* payload key */
    uint64_t counter;
    unsigned char buf[CHUNK];
    size_t fill;
};

/* -------------------------------------------------------------- bech32 -- */

static uint32_t bech32_polymod(const uint8_t *v, size_t n) {
    static const uint32_t gen[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    uint32_t chk = 1;
    for (size_t i = 0; i < n; i++) {
        uint32_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ v[i];
        for (int b = 0; b < 5; b++) if (top >> b & 1) chk ^= gen[b];
    }
    return chk;
}

/* Decodes "age1..." into the 32-byte public key. Returns 0 on success. */
static int decode_recipient(const char *s, unsigned char pk[32]) {
    static const char CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    size_t len = strlen(s);
    if (len != 62 || strncmp(s, "age1", 4)) return -1;
    uint8_t v[62 + 4];                   /* expanded hrp, separator 0, then the 58 data values */
    size_t n = 0;
    for (int i = 0; i < 3; i++) v[n++] = (uint8_t)((unsigned char)"age"[i] >> 5);
    v[n++] = 0;
    for (int i = 0; i < 3; i++) v[n++] = (uint8_t)((unsigned char)"age"[i] & 31);
    uint8_t data[58];
    for (int i = 0; i < 58; i++) {
        const char *p = memchr(CHARSET, s[4 + i], 32);
        if (!p || !s[4 + i]) return -1;  /* (all lower case: mixed case is not accepted) */
        data[i] = v[n++] = (uint8_t)(p - CHARSET);
    }
    if (bech32_polymod(v, n) != 1) return -1;
    uint32_t acc = 0;                    /* 52 values of 5 bits = 260 bits -> 32 bytes + 4 zero padding bits */
    int bits = 0;
    size_t out = 0;
    for (int i = 0; i < 52; i++) {
        acc = acc << 5 | data[i];
        bits += 5;
        if (bits >= 8) { bits -= 8; pk[out++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    return out == 32 && acc == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- HKDF -- */

static void hmac(unsigned char out[32], const unsigned char *key, size_t klen, const unsigned char *a, size_t alen,
                 const unsigned char *b, size_t blen) {
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key, klen);
    crypto_auth_hmacsha256_update(&st, a, alen);
    if (blen) crypto_auth_hmacsha256_update(&st, b, blen);
    crypto_auth_hmacsha256_final(&st, out);
}

/* HKDF-SHA256 with a 32-byte output (one block). */
static void hkdf(unsigned char out[32], const unsigned char *ikm, size_t ilen, const unsigned char *salt, size_t slen,
                 const char *info) {
    unsigned char prk[32], zeros[32] = {0}, t[64];
    hmac(prk, slen ? salt : zeros, slen ? slen : 32, ikm, ilen, NULL, 0);
    size_t il = strlen(info);
    memcpy(t, info, il);
    t[il] = 1;
    hmac(out, prk, 32, t, il + 1, NULL, 0);
    sodium_memzero(prk, sizeof prk);
}

static void b64(char *out, size_t outsz, const unsigned char *in, size_t n) {
    sodium_bin2base64(out, outsz, in, n, sodium_base64_VARIANT_ORIGINAL_NO_PADDING);
}

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* -------------------------------------------------------------- stream -- */

AgeW *age_open(int fd, const char *recipient) {
    unsigned char pk[32];
    if (decode_recipient(recipient, pk)) { errno = EINVAL; return NULL; }

    unsigned char file_key[16], esk[32], share[32], shared[32], salt[64], wrap_key[32], wrapped[16 + TAG], mac_key[32],
                  mac[32], nonce0[12] = {0}, payload_nonce[16];
    randombytes_buf(file_key, sizeof file_key);
    randombytes_buf(esk, sizeof esk);
    crypto_scalarmult_curve25519_base(share, esk);
    if (crypto_scalarmult_curve25519(shared, esk, pk)) { errno = EINVAL; return NULL; }   /* low-order key */
    memcpy(salt, share, 32);
    memcpy(salt + 32, pk, 32);
    hkdf(wrap_key, shared, 32, salt, 64, "age-encryption.org/v1/X25519");
    unsigned long long wl;
    crypto_aead_chacha20poly1305_ietf_encrypt(wrapped, &wl, file_key, sizeof file_key, NULL, 0, NULL, nonce0, wrap_key);

    char share64[48], wrapped64[48], header[256];
    b64(share64, sizeof share64, share, 32);
    b64(wrapped64, sizeof wrapped64, wrapped, sizeof wrapped);
    int hl = snprintf(header, sizeof header, "age-encryption.org/v1\n-> X25519 %s\n%s\n---", share64, wrapped64);
    hkdf(mac_key, file_key, sizeof file_key, NULL, 0, "header");
    hmac(mac, mac_key, 32, (unsigned char *)header, (size_t)hl, NULL, 0);
    char mac64[48];
    b64(mac64, sizeof mac64, mac, 32);

    AgeW *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->fd = fd;
    randombytes_buf(payload_nonce, sizeof payload_nonce);
    hkdf(w->key, file_key, sizeof file_key, payload_nonce, sizeof payload_nonce, "payload");

    char tail[64];
    int tl = snprintf(tail, sizeof tail, " %s\n", mac64);
    if (write_all(fd, header, (size_t)hl) || write_all(fd, tail, (size_t)tl) ||
        write_all(fd, payload_nonce, sizeof payload_nonce)) {
        int e = errno; free(w); errno = e; return NULL;
    }
    sodium_memzero(file_key, sizeof file_key);
    sodium_memzero(esk, sizeof esk);
    sodium_memzero(shared, sizeof shared);
    sodium_memzero(wrap_key, sizeof wrap_key);
    return w;
}

static int flush_chunk(AgeW *w, int final) {
    unsigned char nonce[12] = {0}, out[CHUNK + TAG];
    for (int i = 0; i < 8; i++) nonce[10 - i] = (unsigned char)(w->counter >> (8 * i));   /* 11-byte big-endian counter */
    nonce[11] = final ? 1 : 0;
    unsigned long long cl;
    crypto_aead_chacha20poly1305_ietf_encrypt(out, &cl, w->buf, w->fill, NULL, 0, NULL, nonce, w->key);
    w->counter++;
    w->fill = 0;
    return write_all(w->fd, out, (size_t)cl);
}

int age_write(AgeW *w, const void *data, size_t n) {
    const unsigned char *p = data;
    while (n) {
        if (w->fill == CHUNK && flush_chunk(w, 0)) return -1;   /* more data follows, so this one is not the last */
        size_t take = CHUNK - w->fill < n ? CHUNK - w->fill : n;
        memcpy(w->buf + w->fill, p, take);
        w->fill += take; p += take; n -= take;
    }
    return 0;
}

int age_close(AgeW *w) {
    int rc = flush_chunk(w, 1);
    int e = errno;
    sodium_memzero(w, sizeof *w);
    free(w);
    errno = e;
    return rc;
}

int age_main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s age1RECIPIENT < plain > encrypted\n", argv[0]); return 2; }
    AgeW *w = age_open(STDOUT_FILENO, argv[1]);
    if (!w) errx(1, "%s: not a valid age public key (it looks like age1... and comes from `age-keygen`)", argv[1]);
    unsigned char buf[16384];
    ssize_t r;
    while ((r = read(STDIN_FILENO, buf, sizeof buf)) > 0)
        if (age_write(w, buf, (size_t)r)) err(1, "write");
    if (r < 0) err(1, "read");
    if (age_close(w)) err(1, "write");
    return 0;
}
