/*
 * daemon.c - the diary itself. One small process that sleeps until something happens:
 *
 *   single tap on the PiSugar button (or a press on a USB button, or `sirius ctl toggle`)  start / stop recording
 *   double tap (or `sirius ctl sync`)                                                      transcribe and upload now
 *
 * A recording runs in a child process. When it stops, a worker child transcribes it, encrypts the results and
 * uploads them; if there is no network the files stay and the upload is tried again later. The e-Paper display shows
 * what the diary is doing; it is refreshed only when that changes, and not more often than every 15 s.
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/input.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "daemon.h"
#include "entry.h"
#include "epd.h"
#include "rec.h"
#include "s3.h"

#define PID_FILE "/run/sirius.pid"
#define PISUGAR_ADDR 0x57
#define PISUGAR_TAP_REG 0x08
#define MIN_DISPLAY_GAP (15.0 * D.speed)            /* seconds between panel refreshes */
#define DISPLAY_TIMEOUT 60.0
#define IDLE_REFRESH 6 * 3600.0                     /* the panel should not show one picture for days */
#define EXIT_OFFLINE 10
#define EXIT_REFUSED 11
#define EXIT_SETUP 12

static struct {
    Config c;
    int recording;
    pid_t rec_pid, worker_pid, disp_pid;
    char rec_dir[1024];
    time_t rec_start;
    double disp_started, last_disp_end, next_sync, retry_delay, next_idle_refresh;
    int sync_wanted, last_upload;                   /* last_upload: -1 unknown, S3_OK / S3_RETRY / S3_FAILED */
    time_t last_upload_time;
    char shown[512], wanted[512], notice[96];
    int i2c_fd, evdev_fd;
    double speed;                                   /* 1; SIRIUS_FAST=1 shortens every wait 60 times, for tests */
} D = {.i2c_fd = -1, .evdev_fd = -1, .last_upload = -1, .speed = 1};

static double mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("sirius: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* -------------------------------------------------------------- display -- */

static void clock_text(char *buf, size_t n, time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%H:%M", &tm);
}

static void compose_screen(char *out, size_t n) {
    StoreCounts k;
    store_counts(&D.c, &k);
    char now[16], when[16];
    clock_text(now, sizeof now, time(NULL));
    if (D.recording) {
        clock_text(when, sizeof when, D.rec_start);
        snprintf(out, n, "!RECORDING\n\nStarted %s\n\nTap again to stop", when);
    } else if (D.worker_pid > 0 && k.to_transcribe > 0) {
        snprintf(out, n, "!SIRIUS\n\nTranscribing %d recording%s\n(a few minutes)\n\nTap to record again",
                 k.to_transcribe, k.to_transcribe == 1 ? "" : "s");
    } else {
        char status[2][64] = {"", ""};
        if (config_missing(&D.c) && !D.c.recipient[0]) snprintf(status[0], sizeof status[0], "NOT SET UP: sudo sirius setup");
        else if (k.to_upload > 0 || k.to_transcribe > 0) {
            snprintf(status[0], sizeof status[0], "Waiting to upload: %d", k.to_upload + k.to_transcribe);
            snprintf(status[1], sizeof status[1], D.last_upload == S3_FAILED ? "Refused - check S3 keys" : "Offline, will retry");
            if (D.last_upload == S3_OK || D.last_upload == -1) snprintf(status[1], sizeof status[1], "Will upload soon");
        } else {
            snprintf(status[0], sizeof status[0], "Nothing waiting");
            if (D.last_upload_time) {
                clock_text(when, sizeof when, D.last_upload_time);
                snprintf(status[1], sizeof status[1], "Last upload %s", when);
            }
        }
        char updated[32];                           /* when this picture was made: the panel does not tick on its own */
        snprintf(updated, sizeof updated, "Updated %s", now);
        snprintf(out, n, "!SIRIUS\n%s\nTap the button to record\n\n%s\n%s\n%s", D.notice[0] ? D.notice : "", status[0],
                 status[1], k.stuck ? "Stuck recordings: see log" : updated);
    }
}

static void want_screen(void) { compose_screen(D.wanted, sizeof D.wanted); }

/* Starts a panel refresh if the picture changed and the last one is old enough. */
static void display_tick(int force) {
    double now = mono();
    if (D.disp_pid > 0 && now - D.disp_started > DISPLAY_TIMEOUT) {
        say("the display did not answer; skipping this update");
        kill(D.disp_pid, SIGKILL);
        return;                                     /* reaped through SIGCHLD */
    }
    if (D.disp_pid > 0 || !strcmp(D.wanted, D.shown)) return;
    if (!force && D.last_disp_end && now - D.last_disp_end < MIN_DISPLAY_GAP) return;
    epd_draw_screen(D.wanted, D.c.flip);
    if (getenv("SIRIUS_PREVIEW")) epd_write_preview(getenv("SIRIUS_PREVIEW"));   /* tests without a panel */
    pid_t pid = epd_update_start();
    memcpy(D.shown, D.wanted, sizeof D.shown);
    if (pid > 0) { D.disp_pid = pid; D.disp_started = now; }
    else D.last_disp_end = now;
}

/* ---------------------------------------------------------- recording -- */

static volatile sig_atomic_t rec_stop;
static void on_rec_term(int s) { (void)s; rec_stop = 1; }

static void start_recording(void) {
    char id[64];
    if (entry_create(&D.c, id, sizeof id, D.rec_dir, sizeof D.rec_dir)) {
        say("cannot create an entry in %s: %s", D.c.data_dir, strerror(errno));
        snprintf(D.notice, sizeof D.notice, "Cannot save: disk problem");
        return;
    }
    char audio[1100];
    snprintf(audio, sizeof audio, "%s/audio.enc", D.rec_dir);
    D.notice[0] = 0;
    pid_t pid = fork();
    if (pid < 0) { say("fork: %s", strerror(errno)); rmdir(D.rec_dir); return; }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        struct sigaction sa = {.sa_handler = on_rec_term};  /* no SA_RESTART: the signal interrupts the read */
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        mlockall(MCL_FUTURE);                           /* what we hold in memory is never written to swap */
        char err[300] = "";
        int rc = rec_run(D.c.mic[0] ? D.c.mic : NULL, audio, &rec_stop, err, sizeof err);
        if (rc) fprintf(stderr, "sirius: recording: %s\n", err);
        _exit(rc == -1 ? 2 : rc == -2 ? 3 : 0);
    }
    D.rec_pid = pid;
    D.recording = 1;
    D.rec_start = time(NULL);
    say("recording %s", id);
}

static void stop_recording(void) {
    if (D.rec_pid > 0) kill(D.rec_pid, SIGTERM);        /* finished off in on_child() */
}

static void toggle(void) {
    if (D.recording) stop_recording();
    else if (!D.rec_pid) start_recording();
    want_screen();
}

/* ------------------------------------------------------------- syncing -- */

static void start_sync(void) {
    if (D.worker_pid > 0 || D.recording) return;
    D.sync_wanted = 0;
    D.next_sync = 0;
    pid_t pid = fork();
    if (pid < 0) { say("fork: %s", strerror(errno)); return; }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setpriority(PRIO_PROCESS, 0, 15);               /* a recording started meanwhile must not stutter */
        mlockall(MCL_FUTURE);                           /* decrypted audio and text stay out of swap */
        char err[400] = "";
        if (store_transcribe(&D.c, err, sizeof err) < 0) { say("%s", err); _exit(EXIT_SETUP); }
        int sent, rc = store_upload(&D.c, &sent, err, sizeof err);
        if (sent) say("uploaded %d file(s)", sent);
        if (rc != S3_OK) say("upload: %s", err);
        _exit(rc == S3_OK ? 0 : rc == S3_RETRY ? EXIT_OFFLINE : EXIT_REFUSED);
    }
    D.worker_pid = pid;
}

static void schedule_sync(double delay) {
    double at = mono() + delay * D.speed;
    if (!D.next_sync || at < D.next_sync) D.next_sync = at;
}

static void on_worker_done(int status) {
    StoreCounts k;
    store_counts(&D.c, &k);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 0) {
        D.last_upload = S3_OK;
        D.retry_delay = 0;
        if (k.to_upload == 0) D.last_upload_time = time(NULL);
        if (k.to_upload > 0 || k.to_transcribe > 0) schedule_sync(300);
    } else if (code == EXIT_OFFLINE) {
        D.last_upload = S3_RETRY;
        D.retry_delay = D.retry_delay ? (D.retry_delay * 2 > 1800 ? 1800 : D.retry_delay * 2) : 120;
        schedule_sync(D.retry_delay);
    } else if (code == EXIT_REFUSED) {
        D.last_upload = S3_FAILED;
        schedule_sync(3600);
    } else {
        say("the sync worker ended abnormally (status %d)", status);
        if (k.to_transcribe > 0 || k.to_upload > 0) schedule_sync(600);
    }
}

/* ------------------------------------------------------------- signals -- */

static void on_child(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == D.rec_pid) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            D.rec_pid = 0;
            D.recording = 0;
            if (code == 2) {                            /* never got going: no microphone */
                snprintf(D.notice, sizeof D.notice, "No microphone found");
                char audio[1100];
                snprintf(audio, sizeof audio, "%s/audio.enc", D.rec_dir);
                unlink(audio);
                rmdir(D.rec_dir);
            } else {
                if (code == 3) snprintf(D.notice, sizeof D.notice, "Microphone stopped early");
                say("recording finished");
                D.sync_wanted = 1;
            }
            want_screen();
        } else if (pid == D.worker_pid) {
            D.worker_pid = 0;
            on_worker_done(status);
            want_screen();
        } else if (pid == D.disp_pid) {
            D.disp_pid = 0;
            D.last_disp_end = mono();
            if (!WIFEXITED(status) || WEXITSTATUS(status)) say("display update failed (status %d)", status);
        }
    }
}

/* ------------------------------------------------------------- buttons -- */

/* PiSugar 3: the button's taps are kept in register 0x08 of the battery chip (1 single, 2 double, 3 long). */
static int pisugar_tap(void) {
    if (D.i2c_fd < 0) return 0;
    unsigned char reg = PISUGAR_TAP_REG, v;
    if (write(D.i2c_fd, &reg, 1) != 1 || read(D.i2c_fd, &v, 1) != 1) return 0;
    int tap = v & 3;
    if (tap) {
        unsigned char w[2] = {reg, (unsigned char)(v & 0xFC)};
        if (write(D.i2c_fd, w, 2) != 2) say("cannot clear the PiSugar tap register");
    }
    return tap;
}

static void open_buttons(void) {
    int fd = open("/dev/i2c-1", O_RDWR | O_CLOEXEC);
    if (fd >= 0 && ioctl(fd, I2C_SLAVE, PISUGAR_ADDR) == 0) {
        unsigned char reg = PISUGAR_TAP_REG, v;
        if (write(fd, &reg, 1) == 1 && read(fd, &v, 1) == 1) { D.i2c_fd = fd; say("PiSugar button found"); }
    }
    if (D.i2c_fd < 0 && fd >= 0) close(fd);
    if (D.c.evdev[0]) {
        D.evdev_fd = open(D.c.evdev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (D.evdev_fd < 0) say("cannot open %s: %s", D.c.evdev, strerror(errno));
        else say("button: %s", D.c.evdev);
    }
    if (D.i2c_fd < 0 && D.evdev_fd < 0) say("no button found: use `sirius ctl toggle` (e.g. from the PiSugar hooks) or set EVDEV");
}

static void read_evdev(void) {
    struct input_event ev[16];
    ssize_t n;
    static double last;
    while ((n = read(D.evdev_fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
            if (ev[i].type == EV_KEY && ev[i].value == 1 && mono() - last > 0.5) { last = mono(); toggle(); }
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) { say("button device went away"); close(D.evdev_fd); D.evdev_fd = -1; }
}

/* ---------------------------------------------------------------- main -- */

int daemon_main(int argc, char **argv) {
    (void)argc; (void)argv;
    char msg[300];
    if (config_load(&D.c, NULL, msg, sizeof msg)) errx(1, "%s", msg);
    signal(SIGPIPE, SIG_IGN);

    sigset_t mask;
    sigemptyset(&mask);
    int sigs[] = {SIGUSR1, SIGUSR2, SIGTERM, SIGINT, SIGCHLD, SIGHUP};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaddset(&mask, sigs[i]);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0) err(1, "signalfd");

    FILE *pf = fopen(PID_FILE, "w");
    if (pf) { fprintf(pf, "%d\n", getpid()); fclose(pf); }
    mkdir(D.c.data_dir, 0700);
    open_buttons();
    say("ready (data in %s)", D.c.data_dir);

    D.sync_wanted = 0;
    if (getenv("SIRIUS_FAST")) D.speed = 1.0 / 60;
    schedule_sync(15);                                  /* pick up what a power cut or a missing network left behind */
    D.next_idle_refresh = mono() + IDLE_REFRESH * D.speed;
    want_screen();
    display_tick(1);

    int quit = 0;
    double next_tap = 0;
    while (!quit) {
        double now = mono(), wake = now + 60;
        if (D.i2c_fd >= 0 && next_tap < wake) wake = next_tap > now ? next_tap : now;
        if (D.next_sync && D.next_sync < wake) wake = D.next_sync;
        if (D.next_idle_refresh < wake) wake = D.next_idle_refresh;
        if (D.disp_pid > 0) { if (now + 1 < wake) wake = now + 1; }
        else if (strcmp(D.wanted, D.shown) && D.last_disp_end && D.last_disp_end + MIN_DISPLAY_GAP < wake)
            wake = D.last_disp_end + MIN_DISPLAY_GAP;
        int timeout_ms = wake > now ? (int)((wake - now) * 1000) + 1 : 0;

        struct pollfd fds[2] = {{sfd, POLLIN, 0}, {D.evdev_fd, POLLIN, 0}};
        poll(fds, D.evdev_fd >= 0 ? 2 : 1, timeout_ms);

        if (fds[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == sizeof si) {
                switch (si.ssi_signo) {
                case SIGUSR1: toggle(); break;
                case SIGUSR2: D.sync_wanted = 1; break;
                case SIGCHLD: on_child(); break;
                case SIGHUP: if (!config_load(&D.c, NULL, msg, sizeof msg)) say("settings reloaded"); else say("%s", msg); break;
                default: quit = 1;
                }
            }
        }
        if (D.evdev_fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) read_evdev();
        now = mono();
        if (D.i2c_fd >= 0 && now >= next_tap) {
            next_tap = now + 0.3;
            switch (pisugar_tap()) {
            case 1: toggle(); break;
            case 2: D.sync_wanted = 1; say("double tap: syncing"); break;
            default: break;
            }
        }
        if (D.next_sync && now >= D.next_sync) D.sync_wanted = 1;
        if (D.sync_wanted && !D.recording && !D.worker_pid) { start_sync(); want_screen(); }
        if (now >= D.next_idle_refresh) {               /* never leave one picture on the panel for days */
            D.next_idle_refresh = now + IDLE_REFRESH * D.speed;
            want_screen();
            D.shown[0] = 0;
        }
        display_tick(0);
    }

    say("shutting down");
    if (D.rec_pid > 0) { kill(D.rec_pid, SIGTERM); waitpid(D.rec_pid, NULL, 0); }   /* finish the recording file properly */
    if (D.worker_pid > 0) { kill(D.worker_pid, SIGTERM); waitpid(D.worker_pid, NULL, 0); }
    if (D.disp_pid > 0) waitpid(D.disp_pid, NULL, 0);
    snprintf(D.wanted, sizeof D.wanted, "!SIRIUS\n\nPowered off\n\nPress the power button\nto wake up");
    D.shown[0] = 0;
    display_tick(1);
    if (D.disp_pid > 0) waitpid(D.disp_pid, NULL, 0);
    unlink(PID_FILE);
    return 0;
}

int ctl_main(int argc, char **argv) {
    int sig = argc == 2 && !strcmp(argv[1], "toggle") ? SIGUSR1 : argc == 2 && !strcmp(argv[1], "sync") ? SIGUSR2 : 0;
    if (!sig) { fprintf(stderr, "usage: %s toggle|sync\n", argv[0]); return 2; }
    FILE *f = fopen(PID_FILE, "r");
    int pid = 0;
    if (!f || fscanf(f, "%d", &pid) != 1 || pid <= 0) { fprintf(stderr, "%s: the diary is not running\n", argv[0]); return 1; }
    fclose(f);
    if (kill(pid, sig)) { perror("kill"); return 1; }
    return 0;
}
