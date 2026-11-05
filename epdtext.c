/*
 * epdtext.c - show UTF-8 text on a Waveshare 2.13" e-Paper HAT (V4, 250x122, black/white).
 *
 * usage: epdtext [-r] [-p out.pbm] [--] [text ...]      (reads stdin when no text is given)
 *        epdtext -l [-i seconds] [-t seconds] [-r] [-p out.pbm]       (loop mode)
 *   -l         loop: every line read from stdin is added to the text on the screen; the screen is refreshed
 *              when new text has arrived, at most once every -i seconds (default 180, Waveshare's advice).
 *              Every update runs in its own process and is abandoned after -t seconds (default 60).
 *   -r         turn the picture by 180 degrees (use it if the text appears upside down)
 *   -p FILE    write a preview image (PBM) of what would be shown, and leave the display alone
 *
 * The text is word-wrapped into 30 columns x 7 rows of 8x16 glyphs (Latin and Cyrillic). If it does not
 * fit, the oldest words are dropped and a leading "…" marks the cut.
 *
 * Built with -DHAVE_EPD it drives the panel through Waveshare's C driver (lgpio backend); without it only
 * -p works ("make preview").
 */
#define _POSIX_C_SOURCE 200809L
#include <err.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_EPD
#include "DEV_Config.h"
#include "EPD_2in13_V4.h"
#endif

#include "font8x16.h"

#define PANEL_W 122                         /* the panel's native (portrait) size */
#define PANEL_H 250
#define ROW_BYTES ((PANEL_W + 7) / 8)
#define W PANEL_H                           /* the picture we draw is landscape */
#define H PANEL_W
#define GLYPH_W 8
#define GLYPH_H 16
#define COLS 30
#define ROWS 7
#define MARGIN_X ((W - COLS * GLYPH_W) / 2)
#define MARGIN_Y ((H - ROWS * GLYPH_H) / 2)
#define ELLIPSIS 0x2026
#define MAX_INPUT (64 * 1024)

_Static_assert(MARGIN_X >= 0 && MARGIN_Y >= 0, "text area larger than the picture");

static uint8_t panel[ROW_BYTES * PANEL_H];  /* what the driver gets: 1 bit per pixel, 1 = white */
static uint8_t preview[H][W];               /* the same picture, landscape, 1 = black */
static int flip;

/* ------------------------------------------------------------- drawing -- */

/* Blackens pixel (x, y) of the landscape picture. The mapping onto the panel's native layout is the same
 * as Waveshare's Python driver uses for landscape images (a 90-degree turn); -r turns it by 180 degrees. */
static void set_black(int x, int y) {
    int nx = flip ? PANEL_W - 1 - y : y;
    int ny = flip ? x : PANEL_H - 1 - x;
    panel[ny * ROW_BYTES + nx / 8] &= (uint8_t)~(0x80 >> (nx % 8));
    preview[y][x] = 1;
}

static const uint8_t *glyph(uint32_t cp) {
    for (int pass = 0; pass < 2; pass++) {      /* second pass: fall back to '?' */
        int lo = 0, hi = FONT_N - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (FONT_CP[mid] == cp) return FONT_BITS[mid];
            if (FONT_CP[mid] < cp) lo = mid + 1; else hi = mid - 1;
        }
        cp = '?';
    }
    return FONT_BITS[0];
}

static void draw_glyph(int col, int row, uint32_t cp) {
    const uint8_t *g = glyph(cp);
    for (int y = 0; y < GLYPH_H; y++)
        for (int x = 0; x < GLYPH_W; x++)
            if (g[y] & (0x80 >> x)) set_black(MARGIN_X + col * GLYPH_W + x, MARGIN_Y + row * GLYPH_H + y);
}

/* ---------------------------------------------------------------- text -- */

static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t cp;
    int n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0F; n = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07; n = 4; }
    else { *p = s + 1; return 0xFFFD; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p = s + i; return 0xFFFD; }   /* also stops at the terminating NUL */
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *p = s + n;
    return cp;
}

typedef struct { const uint32_t *cp; int n; } Word;
typedef struct { uint32_t cp[COLS]; int n; } Line;

/* Wraps words[first..] (preceded by "…" when first > 0) into lines. Returns the number of lines used,
 * or ROWS + 1 as soon as the text would not fit. Words longer than a line are broken. */
static int layout(const Word *words, int nwords, int first, Line *lines) {
    static const uint32_t ellipsis[1] = {ELLIPSIS};
    int nl = 1;
    lines[0].n = 0;
    for (int k = first > 0 ? first - 1 : 0; k < nwords; k++) {
        Word w = k < first ? (Word){ellipsis, 1} : words[k];
        for (int off = 0; off < w.n;) {
            int len = w.n - off > COLS ? COLS : w.n - off;
            Line *l = &lines[nl - 1];
            if (l->n && l->n + 1 + len > COLS) {
                if (nl == ROWS) return ROWS + 1;
                l = &lines[nl++];
                l->n = 0;
            }
            if (l->n) l->cp[l->n++] = ' ';
            memcpy(l->cp + l->n, w.cp + off, len * sizeof *w.cp);
            l->n += len;
            off += len;
        }
    }
    return nl;
}

static void render_text(const char *text) {
    memset(panel, 0xFF, sizeof panel);              /* start from a white picture */
    memset(preview, 0, sizeof preview);
    size_t cap = strlen(text) + 1;
    uint32_t *cps = malloc(cap * sizeof *cps);
    Word *words = malloc(cap * sizeof *words);
    if (!cps || !words) errx(1, "out of memory");

    int ncp = 0, nwords = 0, in_word = 0;           /* split into whitespace-separated words */
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t cp = utf8_next(&p);
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0) { in_word = 0; continue; }
        if (!in_word) { words[nwords++] = (Word){cps + ncp, 0}; in_word = 1; }
        cps[ncp++] = cp;
        words[nwords - 1].n++;
    }

    Line lines[ROWS];
    int first = 0, nl;
    while ((nl = layout(words, nwords, first, lines)) > ROWS) first++;   /* drop the oldest word until it fits */

    for (int r = 0; r < nl; r++)
        for (int c = 0; c < lines[r].n; c++) draw_glyph(c, r, lines[r].cp[c]);
    free(cps);
    free(words);
}

static char *read_stdin(void) {
    char *buf = malloc(MAX_INPUT + 1);
    if (!buf) errx(1, "out of memory");
    size_t n = fread(buf, 1, MAX_INPUT, stdin);
    buf[n] = 0;
    return buf;
}

/* -------------------------------------------------------------- output -- */

static void write_preview(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) err(1, "%s", path);
    fprintf(f, "P4\n%d %d\n", W, H);
    for (int y = 0; y < H; y++) {
        uint8_t row[(W + 7) / 8] = {0};
        for (int x = 0; x < W; x++) if (preview[y][x]) row[x / 8] |= (uint8_t)(0x80 >> (x % 8));
        fwrite(row, 1, sizeof row, f);
    }
    if (fclose(f)) err(1, "%s", path);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void sleep_s(double s) {
    if (s <= 0) return;
    struct timespec ts = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
    nanosleep(&ts, NULL);
}

#ifdef HAVE_EPD
/* Sends the picture to the panel: power up, update, sleep, power down. */
static void show(void) {
    if (DEV_Module_Init() != 0) errx(1, "cannot open the GPIO/SPI interface (is SPI enabled? try sudo)");
    EPD_2in13_V4_Init();
    EPD_2in13_V4_Display(panel);
    EPD_2in13_V4_Sleep();           /* keeps the picture, uses no power */
    DEV_Delay_ms(2000);             /* Waveshare: "important, at least 2s" between sleep and power-off */
    DEV_Module_Exit();
#ifdef USE_LGPIO_LIB
    /* With the lgpio backend Waveshare's Exit() does nothing; cut the panel's power as their Python driver does */
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_PWR_PIN, 0);
#endif
}

/* One update in a process of its own. Waveshare's lgpio backend cannot be started twice in one process (the
 * second DEV_Module_Init leaves the BUSY line unclaimed and the busy-wait never ends), and a child process
 * can be abandoned if the panel stops answering. */
static void show_in_child(double timeout) {
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) err(1, "fork");
    if (pid == 0) {
        show();
        fflush(NULL);
        _exit(0);
    }
    for (double deadline = now_s() + timeout;; sleep_s(0.1)) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (!WIFEXITED(status) || WEXITSTATUS(status)) warnx("display update failed");
            return;
        }
        if (now_s() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            warnx("the display did not answer within %.0f s; skipping this update", timeout);
            return;
        }
    }
}
#endif

/* Draws `text` and shows it. A positive `timeout` (loop mode) runs the display update in a child process. */
static void refresh(const char *text, const char *preview_path, double timeout) {
    (void)timeout;
    render_text(text);
    if (preview_path) write_preview(preview_path);
#ifdef HAVE_EPD
    else if (timeout > 0) show_in_child(timeout);
    else show();
#endif
}

/* ----------------------------------------------------------- loop mode -- */

/* Each line read from stdin is added to the text on the screen. The screen is refreshed when new text has
 * arrived, but at most once every `interval` seconds: lines that arrive in between are shown together. */
#define MAX_HISTORY 4096                /* the screen shows ~200 characters; keep far more than that */

/* Appends input bytes to the history; a newline ends a line and marks the screen as out of date. */
static void absorb(char *hist, size_t *len, const char *in, size_t n, int *dirty) {
    for (size_t i = 0; i < n; i++) {
        if (*len >= 2 * MAX_HISTORY) {                       /* full: drop the oldest half, at a word boundary */
            const char *sp = memchr(hist + MAX_HISTORY, ' ', *len - MAX_HISTORY);
            size_t from = sp ? (size_t)(sp - hist) : MAX_HISTORY;
            while (from < *len && (hist[from] & 0xC0) == 0x80) from++;   /* never start inside a UTF-8 character */
            memmove(hist, hist + from, *len - from);
            *len -= from;
        }
        if (in[i] == '\n') {
            if (*len && hist[*len - 1] != ' ') hist[(*len)++] = ' ';
            *dirty = 1;
        } else {
            hist[(*len)++] = in[i];
        }
    }
}

static void run_loop(double interval, const char *preview_path, double timeout) {
    char hist[2 * MAX_HISTORY + 1], buf[1024];
    size_t len = 0;
    double last = -1e18;                                     /* time of the last refresh */
    int dirty = 0, eof = 0;
    while (!eof || dirty) {
        double wait = dirty ? last + interval - now_s() : -1;
        if (dirty && wait <= 0) {
            hist[len] = 0;
            refresh(hist, preview_path, timeout);
            last = now_s();
            dirty = 0;
        } else if (eof) {                                    /* input is over: wait out the interval, then show the rest */
            sleep_s(wait);
        } else {
            struct pollfd p = {STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, wait < 0 ? -1 : (int)(wait * 1000) + 1) <= 0) continue;
            ssize_t k = read(STDIN_FILENO, buf, sizeof buf);
            if (k > 0) {
                absorb(hist, &len, buf, (size_t)k, &dirty);
            } else if (k == 0 || errno != EINTR) {
                eof = 1;
                if (len && hist[len - 1] != ' ') dirty = 1;  /* the last line had no newline */
            }
        }
    }
}

static void usage(void) {
    fprintf(stderr, "usage: epdtext [-r] [-p out.pbm] [--] [text ...]\n"
                    "       epdtext -l [-i seconds] [-t seconds] [-r] [-p out.pbm]\n"
                    "  shows text on the e-Paper display (reads stdin when no text is given)\n"
                    "  -l       loop: add every line read from stdin to the text on the screen\n"
                    "  -i SEC   loop mode: refresh at most every SEC seconds (default 180, Waveshare's advice)\n"
                    "  -t SEC   loop mode: give up on a display update that takes longer than SEC seconds (default 60)\n"
                    "  -r       turn the picture by 180 degrees\n"
                    "  -p FILE  write a preview image instead of using the display\n");
    exit(2);
}

int epdtext_main(int argc, char **argv) {
    const char *preview_path = NULL;
    double interval = 180, timeout = 60;
    int loop = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-r")) flip = 1;
        else if (!strcmp(argv[i], "-l")) loop = 1;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atof(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atof(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) preview_path = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else usage();
    }
#ifndef HAVE_EPD
    if (!preview_path) errx(1, "built without display support: use -p FILE, or build with HAVE_EPD (the Pi build)");
#endif
    if (loop) {
        if (i < argc) usage();                                /* loop mode takes its text from stdin */
        run_loop(interval > 0 ? interval : 0, preview_path, timeout > 0 ? timeout : 60);
        return 0;
    }

    char *text;
    if (i < argc) {                                       /* text on the command line */
        size_t len = 1;
        for (int k = i; k < argc; k++) len += strlen(argv[k]) + 1;
        text = malloc(len);
        if (!text) errx(1, "out of memory");
        text[0] = 0;
        for (int k = i; k < argc; k++) { if (k > i) strcat(text, " "); strcat(text, argv[k]); }
    } else {
        text = read_stdin();
    }

    refresh(text, preview_path, 0);
    free(text);
    return 0;
}
