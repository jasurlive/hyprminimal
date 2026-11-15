// Bar network speed (waybar custom/net): whichever direction is busier right now, download or upload.
// Replaces the bash loop in scripts/netspeed.sh: no fork per tick, and a line is printed only when the
// text changes, so an idle link does not redraw (and re-blur) the bar every 2 s.
// Built by bin/hyprminimal into .generated/bin/netspeed; scripts/netspeed.sh execs it when present.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>

#define PERIOD_S 2
#define DOWN "\U000F0120"
#define UP   "\U000F011D"

static int read_small(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -1;
    buf[r] = 0;
    return (int)r;
}

static int read_u64(const char *path, unsigned long long *v) {
    char b[32];
    if (read_small(path, b, sizeof b) <= 0) return -1;
    char *e;
    *v = strtoull(b, &e, 10);
    return e == b ? -1 : 0;
}

// interface of the default route, from the kernel routing table
static int default_iface(char *dev, size_t n) {
    FILE *f = fopen("/proc/net/route", "re");
    if (!f) return -1;
    char line[256], ifc[64], dst[16];
    int found = -1;
    if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }   // header
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63s %15s", ifc, dst) == 2 && strcmp(dst, "00000000") == 0) {
            snprintf(dev, n, "%s", ifc);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

// bytes/s -> "863 B/s", "1.2 kB/s", "4.7 MB/s" (same rounding as the old script)
static void rate(unsigned long long b, char *out, size_t n) {
    if (b < 1000ULL)         snprintf(out, n, "%llu B/s", b);
    else if (b < 1000000ULL) snprintf(out, n, "%llu.%llu kB/s", b / 1000, b % 1000 / 100);
    else                     snprintf(out, n, "%llu.%llu MB/s", b / 1000000, b % 1000000 / 100000);
}

static const char *icon_scale(void) {   // icon size / text size, set by Settings
    static char pct[16];
    const char *dir = getenv("HYPRMINIMAL_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/.generated/icon_scale", dir ? dir : ".");
    if (read_small(path, pct, sizeof pct) > 0) {
        pct[strcspn(pct, "\r\n")] = 0;
        if (pct[0] && strspn(pct, "0123456789") == strlen(pct)) return pct;
    }
    return "125";
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void) {
    signal(SIGPIPE, SIG_DFL);                 // waybar gone: die on the next write
    prctl(PR_SET_TIMERSLACK, 50 * 1000 * 1000UL);   // 50 ms slack: the kernel may batch our wakeup with others
    pid_t parent = getppid();
    char dev[64] = "", path[128], last[512] = "", line[512];
    unsigned long long prx = 0, ptx = 0, rx, tx;
    int have_prev = 0;
    double pt = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (;;) {
        if (getppid() != parent) return 0;    // waybar died: don't linger as an orphan
        char cur[64];
        if (default_iface(cur, sizeof cur) != 0) cur[0] = 0;
        if (strcmp(cur, dev) != 0) { snprintf(dev, sizeof dev, "%s", cur); have_prev = 0; }

        line[0] = 0;
        if (!dev[0]) {
            snprintf(line, sizeof line, "{\"text\":\"%s 0 B/s\",\"class\":\"offline\",\"tooltip\":\"Offline\"}\n", DOWN);
            have_prev = 0;
        } else {
            snprintf(path, sizeof path, "/sys/class/net/%s/statistics/rx_bytes", dev);
            int ok = read_u64(path, &rx) == 0;
            snprintf(path, sizeof path, "/sys/class/net/%s/statistics/tx_bytes", dev);
            ok = ok && read_u64(path, &tx) == 0;
            double t = now_s();
            if (!ok) { dev[0] = 0; have_prev = 0; }
            else {
                if (have_prev) {
                    double dt = t - pt;
                    if (dt < 0.5) dt = PERIOD_S;
                    unsigned long long d = rx >= prx ? (unsigned long long)((rx - prx) / dt) : 0;
                    unsigned long long u = tx >= ptx ? (unsigned long long)((tx - ptx) / dt) : 0;
                    char rd[32], ru[32], rv[32];
                    rate(d, rd, sizeof rd); rate(u, ru, sizeof ru);
                    int up = u > d;
                    rate(up ? u : d, rv, sizeof rv);
                    snprintf(line, sizeof line,
                             "{\"text\":\"<span size='%s%%' rise='-1pt'>%s</span>  %s\",\"class\":\"%s\","
                             "\"tooltip\":\"Download %s\\nUpload %s\"}\n",
                             icon_scale(), up ? UP : DOWN, rv, up ? "up" : "down", rd, ru);
                }
                prx = rx; ptx = tx; pt = t; have_prev = 1;
            }
        }
        if (line[0] && strcmp(line, last) != 0) {   // unchanged text: say nothing, the bar stays still
            if (fputs(line, stdout) == EOF || fflush(stdout) == EOF) return 0;
            snprintf(last, sizeof last, "%s", line);
        }
        next.tv_sec += PERIOD_S;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {}
    }
}
