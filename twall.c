#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>

#include "config.h"

#define VERSION "0.2"
#define RESET_UI "\033[?1049l\033[?25h"

static int random_mode = 0;
static char *output = NULL;
static char **outputs = NULL;
static size_t noutputs = 0, capoutputs = 0;

static struct termios orig_termios;
static volatile sig_atomic_t raw_enabled = 0;
static int in_ui = 0;
static int ttyfd = -1;
static FILE *ui = NULL;
static int ui_dirty = 1;

static void
restore_terminal(void)
{
    if (raw_enabled && ttyfd >= 0) {
        tcsetattr(ttyfd, TCSANOW, &orig_termios);
        raw_enabled = 0;
    }
}

static void
enable_raw(void)
{
    if (ttyfd < 0 || tcgetattr(ttyfd, &orig_termios) == -1)
        return;
    struct termios t = orig_termios;
    cfmakeraw(&t);
    tcsetattr(ttyfd, TCSANOW, &t);
    raw_enabled = 1;
}

static void
enter_ui(void)
{
    enable_raw();
    fputs("\033[?1049h\033[?25l", ui);
    fflush(ui);
    in_ui = 1;
}

static void
leave_ui(void)
{
    if (in_ui) {
        fputs(RESET_UI, ui);
        fflush(ui);
        in_ui = 0;
    }
    restore_terminal();
}

static void
die(const char *fmt, ...)
{
    va_list ap;

    leave_ui();
    fputs("twall: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *
xrealloc(void *p, size_t n)
{
    p = realloc(p, n);
    if (!p)
        die("out of memory");
    return p;
}

static char *
xstrdup(const char *s)
{
    char *p = strdup(s);

    if (!p)
        die("out of memory");
    return p;
}

/* wakes poll on resize */
static void
handle_winch(int sig)
{
    (void)sig;
}

static void
handle_signal(int sig)
{
    static const char reset[] = RESET_UI;
    ssize_t r;

    if (ttyfd >= 0)
        r = write(ttyfd, reset, sizeof reset - 1);
    (void)r;
    if (raw_enabled && ttyfd >= 0)
        tcsetattr(ttyfd, TCSANOW, &orig_termios);
    _exit(128 + sig);
}

#define KEY_UP     1000
#define KEY_DOWN   1001
#define KEY_RIGHT  1002
#define KEY_LEFT   1003
#define KEY_HOME   1004
#define KEY_END    1005
#define KEY_PGUP   1006
#define KEY_PGDN   1007
#define KEY_ESC    27

static int
wait_fd(int fd, int ms)
{
    fd_set rfds;
    struct timeval tv;
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR)
            continue;
        return r > 0;
    }
}

static int
read_key(void)
{
    unsigned char c;
    ssize_t r;

    if (ttyfd < 0)
        return -1;
    r = read(ttyfd, &c, 1);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return -1;
    if (c != 0x1b)
        return c;

    if (!wait_fd(ttyfd, 50))
        return KEY_ESC;

    unsigned char seq[8];
    int n = 0;
    do {
        r = read(ttyfd, &seq[n], 1);
    } while (r < 0 && errno == EINTR);
    if (r != 1)
        return 0; /* Alt+key, swallow */
    if (seq[n] != '[' && seq[n] != 'O')
        return 0;
    int prefix = seq[n++];

    while (n < (int)sizeof(seq)) {
        if (!wait_fd(ttyfd, 50))
            break;
        do {
            r = read(ttyfd, &seq[n], 1);
        } while (r < 0 && errno == EINTR);
        if (r != 1)
            break;
        unsigned char b = seq[n++];
        if (b >= 0x40 && b <= 0x7e)
            break;
    }
    unsigned char final = seq[n - 1];
    if (prefix == 'O') {
        switch (final) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        }
        return 0;
    }

    switch (final) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~': {
        int num = 0;
        for (int i = 1; i < n - 1; i++) {
            if (seq[i] >= '0' && seq[i] <= '9')
                num = num * 10 + (seq[i] - '0');
            else
                break;
        }
        switch (num) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 5: return KEY_PGUP;
        case 6: return KEY_PGDN;
        }
        return 0;
    }
    }
    return 0;
}

static char *
state_path(void)
{
    static char buf[PATH_MAX];
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] == '/')
        snprintf(buf, sizeof(buf), "%s/twall/last", xdg);
    else
        snprintf(buf, sizeof(buf), "%s/.local/state/twall/last",
                 home ? home : ".");
    return buf;
}

static int
mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int
save_last(const char *path)
{
    char *p = state_path();
    char dir[PATH_MAX], tmp[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", p);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    if (mkdir_p(dir) != 0) {
        fprintf(stderr, "twall: cannot save state: %s\n", strerror(errno));
        return -1;
    }
    if (snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", p, (long)getpid()) >=
        (int)sizeof(tmp)) {
        fprintf(stderr, "twall: cannot save state: path too long\n");
        return -1;
    }

    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "twall: cannot save state: %s\n", strerror(errno));
        return -1;
    }
    int bad = fprintf(f, "%s\n", path) < 0;
    if (fclose(f) != 0)
        bad = 1;
    if (bad) {
        fprintf(stderr, "twall: cannot save state: %s\n", strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, p) != 0) {
        fprintf(stderr, "twall: cannot save state: %s\n", strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

static char *
read_last(void)
{
    static char buf[PATH_MAX];
    FILE *f = fopen(state_path(), "r");
    if (!f) return NULL;
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';
    return buf;
}

static char **files = NULL;
static size_t nfiles = 0, capfiles = 0;

static int
natcmp(const char *a, const char *b)
{
    const char *oa = a, *ob = b;

    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *as = a, *bs = b;
            while (*as == '0') as++;
            while (*bs == '0') bs++;
            const char *ae = as, *be = bs;
            while (isdigit((unsigned char)*ae)) ae++;
            while (isdigit((unsigned char)*be)) be++;
            size_t al = (size_t)(ae - as), bl = (size_t)(be - bs);
            if (al != bl)
                return al < bl ? -1 : 1;
            int c = memcmp(as, bs, al);
            if (c)
                return c;
            a = ae;
            b = be;
        } else {
            int ca = tolower((unsigned char)*a);
            int cb = tolower((unsigned char)*b);
            if (ca != cb)
                return ca - cb;
            a++;
            b++;
        }
    }
    if (*a)
        return 1;
    if (*b)
        return -1;
    return strcmp(oa, ob);
}

static int
cmp_str(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return natcmp(*pa, *pb);
}

static const char *
base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int
has_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (const char *const *e = exts; *e; e++) {
        if (strcasecmp(dot, *e) == 0)
            return 1;
    }
    return 0;
}

static int
scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *ent;
    int rderr = 0;
    for (;;) {
        errno = 0;
        ent = readdir(d);
        if (!ent) {
            rderr = errno;
            break;
        }
        if (ent->d_name[0] == '.')
            continue;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (!has_ext(ent->d_name)) continue;

        if (nfiles == capfiles) {
            capfiles = capfiles ? capfiles * 2 : 64;
            files = xrealloc(files, capfiles * sizeof(char *));
        }
        files[nfiles++] = xstrdup(path);
    }
    closedir(d);
    if (rderr) {
        errno = rderr;
        return -1;
    }
    if (nfiles > 1)
        qsort(files, nfiles, sizeof(char *), cmp_str);
    return 0;
}

static void
add_output(const char *name)
{
    if (!name || !name[0]) return;
    for (size_t i = 0; i < noutputs; i++)
        if (strcmp(outputs[i], name) == 0) return;
    if (noutputs == capoutputs) {
        capoutputs = capoutputs ? capoutputs * 2 : 8;
        outputs = xrealloc(outputs, capoutputs * sizeof(char *));
    }
    outputs[noutputs++] = xstrdup(name);
}

static void
parse_output_line(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "Monitor ", 8) == 0)
        p += 8;
    else if (p[0] == ':' && p[1] == ' ')
        p += 2;

    char name[128];
    size_t i = 0;
    while (p[i] && !isspace((unsigned char)p[i]) &&
           p[i] != ':' && p[i] != '(' &&
           i < sizeof(name) - 1) {
        name[i] = p[i];
        i++;
    }
    name[i] = '\0';
    if (name[0])
        add_output(name);
}

static void
discover_outputs(void)
{
    char line[512];

    if (!cmd_out[0] || !outputs_cmd || !outputs_cmd[0])
        return;
    FILE *f = popen(outputs_cmd, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
        parse_output_line(line);
    pclose(f);
}

static char filter[256];
static size_t filter_len = 0;
static int filtering = 0;

static int
matches_filter(const char *path)
{
    if (filter_len == 0) return 1;
    const char *b = base_name(path);
    size_t blen = strlen(b);
    if (filter_len > blen) return 0;
    for (size_t i = 0; i + filter_len <= blen; i++) {
        int ok = 1;
        for (size_t j = 0; j < filter_len; j++) {
            char c1 = (char)tolower((unsigned char)b[i + j]);
            char c2 = (char)tolower((unsigned char)filter[j]);
            if (c1 != c2) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static size_t *view = NULL;
static size_t nview = 0, capview = 0;

static void
rebuild_view(void)
{
    nview = 0;
    for (size_t i = 0; i < nfiles; i++) {
        if (!matches_filter(files[i])) continue;
        if (nview == capview) {
            capview = capview ? capview * 2 : 64;
            view = xrealloc(view, capview * sizeof(size_t));
        }
        view[nview++] = i;
    }
}

static char *
expand_cmd_token(const char *src, const char *path)
{
    size_t cap = strlen(src) + (path ? strlen(path) : 0) +
                 (output ? strlen(output) : 0) + 32;
    char *out = xrealloc(NULL, cap);
    size_t oi = 0;
    const char *p = src;
    while (*p) {
        const char *rep = NULL;
        if (p[0] == '%' && p[1] == 's') {
            rep = path ? path : "";
            p += 2;
        } else if (p[0] == '%' && p[1] == 'o') {
            rep = output ? output : "";
            p += 2;
        } else if (p[0] == '%' && p[1] == '%') {
            rep = "%";
            p += 2;
        } else {
            if (oi + 2 > cap) {
                cap *= 2;
                out = xrealloc(out, cap);
            }
            out[oi++] = *p++;
            continue;
        }
        size_t L = strlen(rep);
        if (oi + L + 1 > cap) {
            cap = (cap + L) * 2;
            out = xrealloc(out, cap);
        }
        memcpy(out + oi, rep, L);
        oi += L;
    }
    out[oi] = '\0';
    return out;
}

static int
run_cmd(const char *path)
{
    const char *const *cmdv = output ? cmd_out : cmd;
    if (output && !cmd_out[0]) {
        fprintf(stderr, "twall: -o needs cmd_out in config.h\n");
        return -1;
    }

    char **argv = NULL;
    size_t argc = 0, cap = 0;
    for (size_t i = 0; cmdv[i]; i++) {
        char *arg = expand_cmd_token(cmdv[i], path);
        if (argc == cap) {
            cap = cap ? cap * 2 : 8;
            argv = xrealloc(argv, cap * sizeof(char *));
        }
        argv[argc++] = arg;
    }
    if (argc == 0) {
        free(argv);
        return -1;
    }
    argv = xrealloc(argv, (argc + 1) * sizeof(char *));
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        for (size_t i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }
    int status = 0;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);
    for (size_t i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    if (w < 0)
        return -1; /* waitpid error */
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static void
get_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(ttyfd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static size_t sel = 0;
static size_t offset = 0;

static int
visible_rows(void)
{
    int rows, cols;
    get_size(&rows, &cols);
    int h = rows - ((filtering || filter_len > 0) ? 1 : 0);
    return h > 1 ? h : 1;
}

/* valid UTF-8 length at s, 0 if malformed
   reads ahead only after a valid continuation */
static int
u8len(const unsigned char *s)
{
    unsigned char c = s[0];

    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf)
        return (s[1] & 0xc0) == 0x80 ? 2 : 0;
    if (c >= 0xe0 && c <= 0xef)
        return (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 ? 3 : 0;
    if (c >= 0xf0 && c <= 0xf4)
        return (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
               (s[3] & 0xc0) == 0x80 ? 4 : 0;
    return 0;
}

/* C0, C1, stray bytes and bad UTF-8 become ?
   valid UTF-8 stays whole */
static int
fit(char *buf, const char *s, int max)
{
    int cols = 0;

    if (max > PATH_MAX - 1)
        max = PATH_MAX - 1;
    if (max < 0)
        max = 0;
    while (*s) {
        unsigned char c = (unsigned char)s[0];
        int n;

        if (cols >= max)
            break;
        cols++;
        if (c < 0x20 || c == 0x7f) {
            *buf++ = '?';
            s++;
        } else if (c == 0xc2 && (unsigned char)s[1] >= 0x80 &&
                   (unsigned char)s[1] <= 0x9f) {
            *buf++ = '?';
            s += 2;
        } else if ((n = u8len((const unsigned char *)s)) > 0) {
            for (int i = 0; i < n; i++)
                *buf++ = *s++;
        } else {
            *buf++ = '?';
            s++;
        }
    }
    *buf = '\0';
    return cols;
}

static void
draw2(void)
{
    int rows, cols;
    get_size(&rows, &cols);

    int has_filter_line = filtering || filter_len > 0;
    int top = has_filter_line ? 2 : 1;
    int listh = rows - (top - 1);
    if (listh < 1) listh = 1;

    if (sel >= nview) sel = nview ? nview - 1 : 0;
    if (sel < offset) offset = sel;
    if (nview > 0 && sel >= offset + (size_t)listh)
        offset = sel - (size_t)listh + 1;
    if (nview > 0 && offset + (size_t)listh > nview) {
        offset = nview > (size_t)listh ? nview - (size_t)listh : 0;
        if (sel < offset) offset = sel;
    }

    int scroll = nview > (size_t)listh && cols > 10;
    int room = cols - (scroll ? 3 : 2) - 2;
    if (room < 1) room = 1;

    int thumb = 0, pos = 0;
    if (scroll) {
        thumb = listh * listh / (int)nview;
        if (thumb < 1) thumb = 1;
        pos = (int)(offset * (size_t)(listh - thumb) /
                    (nview - (size_t)listh));
    }

    char buf[PATH_MAX];
    if (has_filter_line) {
        fit(buf, filter, cols - 2);
        fprintf(ui, "\033[1;1H\033[2m/%s\033[0m\033[K", buf);
    }

    for (int i = 0; i < listh; i++) {
        size_t idx = offset + (size_t)i;
        fprintf(ui, "\033[%d;1H", top + i);
        if (idx < nview) {
            const char *name = base_name(files[view[idx]]);
            int w = fit(buf, name, room);
            int is_sel = (idx == sel);
            if (is_sel) fputs("\033[7m", ui);
            fprintf(ui, "  %s", buf);
            if (is_sel) {
                for (; w < room; w++)
                    fputc(' ', ui);
                fputs("\033[0m", ui);
            }
        }
        fputs("\033[K", ui);
        if (scroll) {
            fprintf(ui, "\033[%d;%dH%s", top + i, cols,
                    (i >= pos && i < pos + thumb) ? "\xe2\x96\x88"
                                                  : "\xe2\x94\x82");
        }
    }

    fflush(ui);
}

static void
usage(FILE *out)
{
    fprintf(out,
            "usage: twall [-d DIR] [-o OUT] [-p] [-r] [-R] [-h] [-v]\n"
            "  -d DIR   wallpaper directory\n"
            "  -o OUT   output/monitor for backend\n"
            "  -p       print selected path to stdout, do not apply\n"
            "  -r       restore last wallpaper and exit\n"
            "  -R       random wallpaper, no UI\n"
            "  -h       help\n"
            "  -v       version\n"
            "\n"
            "keys:\n"
            "  j/k, arrows   move\n"
            "  g/G, Home/End first/last\n"
            "  PgUp/PgDn     page\n"
            "  Ctrl-D/Ctrl-U half page\n"
            "  Enter/Space   apply and stay\n"
            "  q/Esc/Ctrl-C  quit\n"
            "  /             filter (Enter keeps, Esc clears)\n"
            "  Tab           cycle output\n"
            "  r             random entry from the visible list\n");
}

static char *
expand_path(const char *p)
{
    if (p && p[0] == '~' && (p[1] == '/' || p[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home) {
            size_t len = strlen(home) + strlen(p) + 1;
            char *buf = xrealloc(NULL, len);
            snprintf(buf, len, "%s%s", home, p + 1);
            return buf;
        }
    }
    return xstrdup(p);
}

/* real %s tokens only, %% is literal */
static int
count_spec(const char *const *v, char spec)
{
    int n = 0;

    if (!v)
        return 0;
    for (size_t i = 0; v[i]; i++) {
        const char *p = v[i];
        while (*p) {
            if (*p != '%') {
                p++;
                continue;
            }
            if (p[1] == '%') {
                p += 2;
                continue;
            }
            if (p[1] == spec)
                n++;
            p += p[1] ? 2 : 1;
        }
    }
    return n;
}

static int
list_height(void)
{
    return visible_rows();
}

int
main(int argc, char **argv)
{
    const char *dir_arg = NULL;
    const char *out_arg = NULL;
    int restore = 0;
    int print_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) { usage(stderr); return 1; }
            dir_arg = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) { usage(stderr); return 1; }
            out_arg = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0) {
            restore = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            print_only = 1;
        } else if (strcmp(argv[i], "-R") == 0) {
            random_mode = 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--version") == 0) {
            printf("twall %s\n", VERSION);
            return 0;
        } else {
            fprintf(stderr, "twall: unknown option %s\n", argv[i]);
            usage(stderr);
            return 1;
        }
    }

    if (print_only + restore + random_mode > 1)
        die("-p, -r and -R are mutually exclusive");

    char *dir = NULL;
    if (dir_arg) {
        dir = expand_path(dir_arg);
    } else if (default_dir) {
        dir = expand_path(default_dir);
    } else {
        const char *home = getenv("HOME");
        char buf[PATH_MAX];
        snprintf(buf, sizeof(buf), "%s/.local/share/backgrounds",
                 home ? home : ".");
        dir = xstrdup(buf);
    }
    if (!dir) die("out of memory");

    /* absolute path, state survives cwd changes */
    char *rp = realpath(dir, NULL);
    if (rp) {
        free(dir);
        dir = rp;
    }

    if (count_spec(cmd, 's') != 1)
        die("config.h: cmd[] needs exactly one token containing %%s");
    if (cmd_out[0]) {
        if (count_spec(cmd_out, 's') != 1)
            die("config.h: cmd_out[] needs exactly one token containing %%s");
        if (count_spec(cmd_out, 'o') < 1)
            die("config.h: cmd_out[] needs at least one token containing %%o");
    }
    if (out_arg && !cmd_out[0])
        die("config.h: -o needs cmd_out");

    if (out_arg)
        output = xstrdup(out_arg);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    if (random_mode) {
        if (scan_dir(dir) != 0) {
            fprintf(stderr, "twall: cannot read directory %s: %s\n", dir, strerror(errno));
            free(dir);
            return 1;
        }
        if (nfiles == 0) {
            fprintf(stderr, "twall: no wallpapers found\n");
            free(dir);
            return 1;
        }
        char *last = read_last();
        size_t idx = (size_t)rand() % nfiles;
        if (nfiles > 1 && last && strcmp(files[idx], last) == 0)
            idx = (idx + 1) % nfiles;
        const char *path = files[idx];
        int rc = run_cmd(path);
        if (rc == 0)
            save_last(path);
        free(dir);
        return rc == 0 ? 0 : 1;
    }

    if (restore) {
        char *last = read_last();
        if (!last || !last[0]) {
            fprintf(stderr, "twall: no last wallpaper saved\n");
            free(dir);
            return 1;
        }
        if (access(last, R_OK) != 0) {
            char disp[PATH_MAX];
            fit(disp, last, sizeof(disp) - 1);
            fprintf(stderr, "twall: last wallpaper %s: %s\n",
                    disp, strerror(errno));
            free(dir);
            return 1;
        }
        int rc = run_cmd(last);
        free(dir);
        return rc == 0 ? 0 : 1;
    }

    if (scan_dir(dir) != 0) {
        fprintf(stderr, "twall: cannot read directory %s: %s\n", dir, strerror(errno));
        free(dir);
        return 1;
    }
    rebuild_view();
    if (nfiles == 0) {
        fprintf(stderr, "twall: no wallpapers found\n");
        free(dir);
        return 1;
    }

    discover_outputs();
    if (out_arg) {
        int found = 0;
        for (size_t i = 0; i < noutputs; i++)
            if (strcmp(outputs[i], out_arg) == 0) { found = 1; break; }
        if (!found) add_output(out_arg);
    }

    ttyfd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (ttyfd < 0)
        die("cannot open /dev/tty");
    ui = fdopen(ttyfd, "w");
    if (!ui)
        die("cannot open terminal for writing");
    setvbuf(ui, NULL, _IOFBF, 1 << 16);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = handle_winch;
    sa.sa_flags = 0; /* no SA_RESTART, poll must wake */
    sigaction(SIGWINCH, &sa, NULL);

    enter_ui();

    int running = 1;
    int last_rows = 0, last_cols = 0;
    while (running) {
        int rows, cols;
        get_size(&rows, &cols);
        if (rows != last_rows || cols != last_cols) {
            last_rows = rows;
            last_cols = cols;
            ui_dirty = 1;
        }

        if (ui_dirty) {
            draw2();
            ui_dirty = 0;
        }

        struct pollfd pfd;
        pfd.fd = ttyfd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int k = read_key();
        if (k == -1) break;
        ui_dirty = 1;

        if (k == '/') {
            filtering = 1;
            filter[0] = '\0';
            filter_len = 0;
            for (;;) {
                rebuild_view();
                if (sel >= nview) sel = nview ? nview - 1 : 0;
                draw2();
                int fk = read_key();
                if (fk == -1) { running = 0; break; }
                if (fk == 3) { running = 0; break; }
                if (fk == '\r' || fk == '\n') break;
                if (fk == KEY_ESC) {
                    filter[0] = '\0';
                    filter_len = 0;
                    rebuild_view();
                    break;
                }
                if (fk == 127 || fk == '\b') {
                    while (filter_len > 0 &&
                           (filter[--filter_len] & 0xc0) == 0x80)
                        ;
                    filter[filter_len] = '\0';
                } else if (fk >= 32 && fk < 256 &&
                           filter_len < sizeof(filter) - 1) {
                    filter[filter_len++] = (char)fk;
                    filter[filter_len] = '\0';
                }
            }
            filtering = 0;
            ui_dirty = 1;
            if (!running) break;
            continue;
        }

        switch (k) {
        case 'q':
        case 3:
        case KEY_ESC:
            running = 0;
            break;
        case 'j':
        case KEY_DOWN:
            if (nview > 0 && sel + 1 < nview) sel++;
            break;
        case 'k':
        case KEY_UP:
            if (sel > 0) sel--;
            break;
        case 'g':
            sel = 0;
            break;
        case 'G':
        case KEY_END:
            if (nview > 0) sel = nview - 1;
            break;
        case KEY_HOME:
            sel = 0;
            break;
        case KEY_PGDN:
        case 4:
            if (nview > 0) {
                size_t step = (size_t)list_height();
                if (k == 4) step = (step + 1) / 2;
                if (sel + step < nview) sel += step; else sel = nview - 1;
            }
            break;
        case KEY_PGUP:
        case 21:
            {
                size_t step = (size_t)list_height();
                if (k == 21) step = (step + 1) / 2;
                if (sel > step) sel -= step; else sel = 0;
            }
            break;
        case 'r':
            if (nview > 0)
                sel = (size_t)rand() % nview;
            break;
        case '\t': {
            if (noutputs == 0)
                break;
            size_t idx = 0;
            if (output) {
                for (size_t i = 0; i < noutputs; i++)
                    if (strcmp(outputs[i], output) == 0) { idx = i + 1; break; }
            }
            idx = (idx + 1) % (noutputs + 1);
            free(output);
            output = idx == 0 ? NULL : xstrdup(outputs[idx - 1]);
            break;
        }
        case '\r':
        case '\n':
        case ' ': {
            if (nview == 0) break;
            const char *path = files[view[sel]];
            if (print_only) {
                leave_ui();
                printf("%s\n", path);
                free(dir);
                return 0;
            }
            if (run_cmd(path) == 0)
                save_last(path);
            break;
        }
        default:
            break;
        }
    }

    leave_ui();
    free(dir);
    return print_only ? 1 : 0;
}
