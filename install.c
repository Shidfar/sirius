/*
 * install.c - everything a clean Raspberry Pi OS needs to run the diary, done by the binary itself:
 *
 *   sudo ./sirius install     copy itself to /usr/local/bin, switch on SPI (display) and I2C (PiSugar button),
 *                             make the data directory, add a systemd service that starts the diary at boot
 *   sudo sirius setup         ask for the age public key and the S3 details, test the upload
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "age.h"
#include "config.h"
#include "install.h"
#include "s3.h"

#define BIN "/usr/local/bin/sirius"
#define CONF "/etc/sirius.conf"
#define UNIT "/etc/systemd/system/sirius.service"
#define DATA "/var/lib/sirius"

static const char *conf_path(void) { return getenv("SIRIUS_CONF") ? getenv("SIRIUS_CONF") : CONF; }

static const char UNIT_TEXT[] =
    "[Unit]\n"
    "Description=Sirius voice diary\n"
    "After=local-fs.target sound.target\n"
    "\n"
    "[Service]\n"
    "ExecStart=" BIN " run\n"
    "WorkingDirectory=" DATA "\n"
    "Restart=on-failure\n"
    "RestartSec=5\n"
    "TimeoutStopSec=40\n"
    "LimitMEMLOCK=infinity\n"
    "NoNewPrivileges=yes\n"
    "PrivateTmp=yes\n"
    "ProtectHome=yes\n"
    "ProtectSystem=strict\n"
    "ReadWritePaths=" DATA " /run\n"
    "\n"
    "[Install]\n"
    "WantedBy=multi-user.target\n";

static int run(const char *const argv[]) {         /* runs a program quietly; returns its exit status or -1 */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 1);
        dup2(nul, 2);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

static int write_file(const char *path, const char *text, mode_t mode) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    int bad = write(fd, text, n) != (ssize_t)n || fsync(fd);
    close(fd);
    if (bad || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

/* Copies this program to `dest` (through a temporary file, so a running copy is replaced safely). */
static int install_binary(void) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n < 0) return -1;
    self[n] = 0;
    char real[PATH_MAX];
    if (realpath(self, real) && !strcmp(real, BIN)) { puts("  the program is already in " BIN); return 0; }
    int in = open(self, O_RDONLY);
    struct stat st;
    if (in < 0 || fstat(in, &st)) return -1;
    struct statvfs vfs;
    if (statvfs("/usr/local/bin", &vfs) == 0 && (unsigned long long)vfs.f_bavail * vfs.f_frsize < (unsigned long long)st.st_size + (50ull << 20)) {
        fprintf(stderr, "not enough free space in /usr/local/bin for %lld MB\n", (long long)st.st_size >> 20);
        return -1;
    }
    mkdir("/usr/local/bin", 0755);
    char tmp[] = BIN ".new";
    unlink(tmp);
    int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (out < 0) return -1;
    char buf[1 << 16];
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0)
        for (char *p = buf; r > 0;) { ssize_t w = write(out, p, (size_t)r); if (w < 0) { close(out); unlink(tmp); return -1; } p += w; r -= w; }
    if (r < 0 || fsync(out) || close(out) || rename(tmp, BIN)) { unlink(tmp); return -1; }
    close(in);
    printf("  copied the program to " BIN " (%lld MB)\n", (long long)st.st_size >> 20);
    return 0;
}

/* Makes sure "name=on" is active in the boot configuration. Returns 1 if the file was changed, 0 if not, -1 on error. */
static int ensure_dtparam(char **text, const char *line) {
    size_t ll = strlen(line);
    for (char *p = *text; p && *p;) {
        char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= ll && !strncmp(p, line, ll) && (len == ll || isspace((unsigned char)p[ll]))) return 0;   /* already on */
        p = eol ? eol + 1 : NULL;
    }
    size_t n = strlen(*text);
    char *grown = realloc(*text, n + ll + 32);
    if (!grown) return -1;
    *text = grown;
    snprintf(*text + n, ll + 32, "%s%s\n", n && (*text)[n - 1] != '\n' ? "\n" : "", line);
    return 1;
}

static int boot_config(int *reboot) {
    const char *path = access("/boot/firmware/config.txt", W_OK) == 0 ? "/boot/firmware/config.txt" : "/boot/config.txt";
    char *text = slurp(path, NULL);
    if (!text) { fprintf(stderr, "  cannot read the boot configuration (%s): %s\n", path, strerror(errno)); return -1; }
    int changed = 0;
    for (const char *const *l = (const char *const[]){"dtparam=spi=on", "dtparam=i2c_arm=on", NULL}; *l; l++) {
        int r = ensure_dtparam(&text, *l);
        if (r < 0) { free(text); return -1; }
        changed |= r;
    }
    if (changed) {
        char backup[300];
        snprintf(backup, sizeof backup, "%s.sirius-backup", path);
        if (access(backup, F_OK)) {
            char *orig = slurp(path, NULL);
            if (orig) { write_file(backup, orig, 0644); free(orig); }
        }
        if (write_file(path, text, 0755 & 0644)) { fprintf(stderr, "  cannot write %s: %s\n", path, strerror(errno)); free(text); return -1; }
        printf("  switched on SPI and I2C in %s (a reboot is needed once)\n", path);
        *reboot = 1;
    } else {
        printf("  SPI and I2C are already on in %s\n", path);
    }
    free(text);
    write_file("/etc/modules-load.d/sirius.conf", "i2c-dev\n", 0644);     /* creates /dev/i2c-1 for the PiSugar button */
    return 0;
}

int install_main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (geteuid() != 0) { fprintf(stderr, "run it as root: sudo %s install\n", argv[0]); return 1; }
    puts("Installing Sirius");
    if (install_binary()) { fprintf(stderr, "  copying the program failed: %s\n", strerror(errno)); return 1; }
    int reboot = 0;
    if (boot_config(&reboot)) return 1;
    mkdir(DATA, 0700);
    chmod(DATA, 0700);
    printf("  data directory %s (only root can read it)\n", DATA);
    if (access(CONF, F_OK)) {
        Config c;
        char e[200];
        config_load(&c, "/nonexistent", e, sizeof e);
        config_save(&c, CONF);
        puts("  made " CONF " (fill it in with: sudo sirius setup)");
    } else {
        puts("  " CONF " already exists, left as it is");
    }
    if (write_file(UNIT, UNIT_TEXT, 0644)) { fprintf(stderr, "  cannot write " UNIT ": %s\n", strerror(errno)); return 1; }
    run((const char *const[]){"systemctl", "daemon-reload", NULL});
    if (run((const char *const[]){"systemctl", "enable", "sirius.service", NULL})) fprintf(stderr, "  could not enable the service\n");
    else puts("  the diary starts by itself at boot (sirius.service)");
    if (!reboot) run((const char *const[]){"systemctl", "restart", "sirius.service", NULL});
    puts("");
    puts("Next:");
    puts("  1. sudo sirius setup         (age public key and S3 bucket, tests the upload)");
    if (reboot) puts("  2. sudo reboot               (the display needs SPI, which was just switched on)");
    puts("  Then plug in the USB microphone and press the button.");
    return 0;
}

/* ---------------------------------------------------------------- setup -- */

static void ask(const char *prompt, const char *current, char *out, size_t n, int hidden) {
    struct termios old, quiet;
    int tty = hidden && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (current && *current) printf("%s [%s]: ", prompt, hidden ? "keep" : current);
    else printf("%s: ", prompt);
    fflush(stdout);
    if (tty) { quiet = old; quiet.c_lflag &= (tcflag_t)~ECHO; tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet); }
    char line[512] = "";
    if (!fgets(line, sizeof line, stdin)) line[0] = 0;
    if (tty) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); putchar('\n'); }
    line[strcspn(line, "\r\n")] = 0;
    if (line[0]) snprintf(out, n, "%s", line);
}

static int valid_recipient(const char *r) {
    int fd = open("/dev/null", O_WRONLY);
    AgeW *w = age_open(fd, r);
    close(fd);
    if (!w) return 0;
    age_close(w);
    return 1;
}

int setup_main(int argc, char **argv) {
    (void)argc;
    if (geteuid() != 0) { fprintf(stderr, "run it as root: sudo %s setup\n", argv[0]); return 1; }
    Config c;
    char e[300];
    if (config_load(&c, conf_path(), e, sizeof e)) { fprintf(stderr, "%s\n", e); return 1; }
    puts("Your diary entries are encrypted on this device with your age PUBLIC key, so only you can read them.");
    puts("(Make the key pair on your own computer with: age-keygen -o sirius-key.txt  - never put the private key here.)\n");
    for (;;) {
        ask("age public key (age1...)", c.recipient, c.recipient, sizeof c.recipient, 0);
        if (valid_recipient(c.recipient)) break;
        printf("That is not an age public key. It starts with age1 and is 62 characters long.\n");
        c.recipient[0] = 0;
    }
    ask("S3 bucket name", c.bucket, c.bucket, sizeof c.bucket, 0);
    ask("S3 region (for example eu-north-1)", c.region, c.region, sizeof c.region, 0);
    ask("Access key ID of the upload-only user", c.access_key, c.access_key, sizeof c.access_key, 0);
    ask("Secret access key", c.secret_key, c.secret_key, sizeof c.secret_key, 1);
    const char *missing = config_missing(&c);
    if (missing) { fprintf(stderr, "%s\n", missing); return 1; }
    if (config_save(&c, conf_path())) { fprintf(stderr, "cannot write %s: %s\n", conf_path(), strerror(errno)); return 1; }
    printf("Saved to %s.\n\n", conf_path());

    puts("Testing the upload ...");
    char path[] = "/tmp/sirius-test-XXXXXX";
    int fd = mkstemp(path);
    AgeW *w = fd >= 0 ? age_open(fd, c.recipient) : NULL;
    if (!w) { fprintf(stderr, "cannot make a test file\n"); return 1; }
    const char *hello = "Sirius setup test\n";
    age_write(w, hello, strlen(hello));
    age_close(w);
    close(fd);
    S3Conf s3 = {c.bucket, c.region, c.access_key, c.secret_key, c.endpoint[0] ? c.endpoint : NULL, c.ca_file[0] ? c.ca_file : NULL};
    char key[300], err[300] = "";
    snprintf(key, sizeof key, "%ssetup-test/%ld.age", c.prefix, (long)time(NULL));
    int rc = s3_put_file(&s3, key, path, err, sizeof err);
    unlink(path);
    if (rc == S3_OK) printf("Works: uploaded %s. Everything is ready; restart with: sudo systemctl restart sirius\n", key);
    else if (rc == S3_RETRY) printf("Could not reach S3 (%s).\nThe settings are saved; check the Wi-Fi and run this again to test.\n", err);
    else printf("S3 refused the upload: %s\nCheck the bucket name, region, keys and that the user may PutObject under %s*.\n", err, c.prefix);
    return rc == S3_FAILED ? 1 : 0;
}
