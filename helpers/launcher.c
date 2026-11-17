// hyprminimal launcher (Super+R): numbered app list in rofi.
// Apps are numbered A-Z (numbers stay the same between runs); the 5 apps launched last are pinned on
// top, newest first. Type a number + Enter to launch that app, or search by name.
// "rm NAME" (or "rm NUMBER") + Enter uninstalls the app instead, in a terminal that asks first.
// The app list is cached; scripts/launcher.py (Python + Gio) only rebuilds it when apps change.
// Replaces scripts/launcher.sh (kept as the fallback when gcc is missing): no shell, no subprocesses
// on the way to rofi besides rofi itself.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KEEP 5
#define TAG "<span alpha='40%' size='small'>   recent</span>"
// rofi ranks by the *display* text (markup too) and costs every character after the match, so other
// rows end in the same tag, invisible: equal length for all, the name alone decides the order
#define NOTAG "<span alpha='1%'  size='small'>         </span>"

static char ROOT[PATH_MAX - 128];   // room for the suffixes appended below

typedef struct { char *id, *file, *search, *shown, *tail; } App;

static char *slurp(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }
    char *buf = malloc(st.st_size + 1);
    size_t n = 0;
    ssize_t r;
    while (buf && n < (size_t)st.st_size && (r = read(fd, buf + n, st.st_size - n)) > 0) n += r;
    close(fd);
    if (!buf) return NULL;
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static int newer(const struct timespec *a, const struct timespec *b) {
    return a->tv_sec > b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

// true when an applications dir (or anything directly in it) or the builder changed after the cache
static int dir_newer(const char *dir, const struct timespec *t) {
    struct stat st;
    if (stat(dir, &st) < 0) return 0;
    if (newer(&st.st_mtim, t)) return 1;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int hit = 0;
    while (!hit && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (fstatat(dirfd(d), e->d_name, &st, 0) == 0 && newer(&st.st_mtim, t)) hit = 1;
    }
    closedir(d);
    return hit;
}

static int stale(const char *cache) {
    struct stat cs, st;
    if (stat(cache, &cs) < 0 || cs.st_size == 0) return 1;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/scripts/launcher.py", ROOT);
    if (stat(p, &st) == 0 && newer(&st.st_mtim, &cs.st_mtim)) return 1;
    const char *h = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (h && *h) snprintf(p, sizeof p, "%s/applications", h);
    else snprintf(p, sizeof p, "%s/.local/share/applications", home);
    if (dir_newer(p, &cs.st_mtim)) return 1;
    const char *dd = getenv("XDG_DATA_DIRS");
    char *dirs = strdup(dd && *dd ? dd : "/usr/local/share:/usr/share"), *save = NULL;
    int hit = 0;
    for (char *s = strtok_r(dirs, ":", &save); s && !hit; s = strtok_r(NULL, ":", &save)) {
        snprintf(p, sizeof p, "%s/applications", s);
        hit = dir_newer(p, &cs.st_mtim);
    }
    free(dirs);
    return hit;
}

static int run_wait(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    if (pid < 0) return -1;
    int st;
    while (waitpid(pid, &st, 0) < 0) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// detached: double fork + setsid, no stdio, so the app outlives us and isn't our child
static void spawn(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() == 0) {
            int n = open("/dev/null", O_RDWR);
            dup2(n, 0); dup2(n, 1); dup2(n, 2);
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

// tell Settings which popup closes on a click outside it ("" = none): hyprminimal-msg catch, minus bash
static pid_t catch_popup(const char *name) {
    char arg[64];
    snprintf(arg, sizeof arg, "[<'%s'>]", name);
    pid_t pid = fork();
    if (pid == 0) {
        int n = open("/dev/null", O_RDWR);
        dup2(n, 1); dup2(n, 2);
        execlp("gdbus", "gdbus", "call", "--session", "--dest", "dev.hyprminimal.Settings", "--object-path",
               "/dev/hyprminimal/Settings", "--method", "org.gtk.Actions.Activate", "catch", arg, "{}",
               (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w; n -= w;
    }
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int all_digits(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

int main(void) {
    const char *env = getenv("HYPRMINIMAL_DIR");
    if (env && *env) snprintf(ROOT, sizeof ROOT, "%s", env);
    else {   // .generated/bin/launcher -> repo
        ssize_t n = readlink("/proc/self/exe", ROOT, sizeof ROOT - 1);
        if (n < 0) return 1;
        ROOT[n] = 0;
        for (int i = 0; i < 3; i++) { char *s = strrchr(ROOT, '/'); if (s) *s = 0; }
    }
    const char *home = getenv("HOME"), *xc = getenv("XDG_CACHE_HOME"), *xs = getenv("XDG_STATE_HOME");
    char cdir[PATH_MAX - 128], cache[PATH_MAX], state[PATH_MAX - 128], recent_path[PATH_MAX], theme[PATH_MAX];
    if (xc && *xc) snprintf(cdir, sizeof cdir, "%s/hyprminimal", xc);
    else snprintf(cdir, sizeof cdir, "%s/.cache/hyprminimal", home);
    snprintf(cache, sizeof cache, "%s/launcher-apps", cdir);
    if (xs && *xs) snprintf(state, sizeof state, "%s/hyprminimal", xs);
    else snprintf(state, sizeof state, "%s/.local/state/hyprminimal", home);
    snprintf(recent_path, sizeof recent_path, "%s/launcher-recent", state);

    pid_t catcher = catch_popup("rofi");   // click outside closes it (runs alongside)

    if (stale(cache)) {
        char py[PATH_MAX];
        snprintf(py, sizeof py, "%s/scripts/launcher.py", ROOT);
        mkdir(cdir, 0755);
        if (run_wait((char *[]){"python3", "-I", py, cache, NULL}) != 0) return 1;
    }

    // cache: id \t file \t search \t shown \t tail (see launcher.py), one app per line, A-Z
    char *data = slurp(cache, NULL);
    if (!data) return 1;
    size_t cap = 256, n = 0;
    App *apps = malloc(cap * sizeof *apps);
    for (char *save = NULL, *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[5];
        int k = 0;
        for (char *p = line; k < 5; k++) {
            f[k] = p;
            char *t = strchr(p, '\t');
            if (!t) { k++; break; }
            *t = 0; p = t + 1;
        }
        if (k < 5) continue;
        if (n == cap) apps = realloc(apps, (cap *= 2) * sizeof *apps);
        apps[n++] = (App){f[0], f[1], f[2], f[3], f[4]};
    }
    if (!n) return 1;

    // recent ids (newest first) -> pinned on top; uninstalled apps drop out
    char *rdata = slurp(recent_path, NULL);
    char *recent_ids[64];
    int nrid = 0;
    if (rdata)
        for (char *save = NULL, *t = strtok_r(rdata, " \t\n", &save); t && nrid < 64; t = strtok_r(NULL, " \t\n", &save))
            recent_ids[nrid++] = t;
    size_t *order = malloc(n * sizeof *order);
    char *pinned = calloc(n, 1);
    size_t no = 0;
    for (int r = 0; r < nrid && no < KEEP; r++)
        for (size_t i = 0; i < n; i++)
            if (!pinned[i] && strcmp(apps[i].id, recent_ids[r]) == 0) { pinned[i] = 1; order[no++] = i; break; }
    size_t nrecent = no;
    for (size_t i = 0; i < n; i++) if (!pinned[i]) order[no++] = i;

    snprintf(theme, sizeof theme, "%s/.generated/rofi/launcher.rasi", ROOT);
    if (access(theme, R_OK) != 0) snprintf(theme, sizeof theme, "%s/rofi/launcher.rasi", ROOT);
    char active[32];
    snprintf(active, sizeof active, "0-%zu", nrecent ? nrecent - 1 : 0);
    char *argv[] = {"rofi", "-dmenu", "-i", "-markup-rows", "-no-show-icons", "-no-custom", "-format", "i\tf",
                    "-p", "\U000F0349", "-theme", theme, nrecent ? "-a" : NULL, active, NULL};

    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0) return 1;
    pid_t rofi = fork();
    if (rofi == 0) {
        dup2(in[0], 0); dup2(out[1], 1);
        execvp("rofi", argv);
        _exit(127);
    }
    close(in[0]); close(out[1]);

    // all rows in one buffer, one write: "search\0display\x1fshown TAG|NOTAG tail\n"
    size_t blen = 0;
    for (size_t r = 0; r < no; r++) {
        App *a = &apps[order[r]];
        blen += strlen(a->search) + strlen(a->shown) + strlen(a->tail) + sizeof TAG + 16;
    }
    char *buf = malloc(blen), *w = buf;
    for (size_t r = 0; r < no; r++) {
        App *a = &apps[order[r]];
        w = stpcpy(w, a->search);
        *w++ = 0;
        w = stpcpy(w, "display\x1f");
        w = stpcpy(w, a->shown);
        w = stpcpy(w, r < nrecent ? TAG : NOTAG);
        w = stpcpy(w, a->tail);   // invisible: ranks apps starting with the typed text first
        *w++ = '\n';
    }
    write_all(in[1], buf, w - buf);
    close(in[1]);

    char res[4096];
    size_t rl = 0;
    ssize_t r;
    while (rl < sizeof res - 1 && (r = read(out[0], res + rl, sizeof res - 1 - rl)) > 0) rl += r;
    res[rl] = 0;
    close(out[0]);
    int st;
    while (waitpid(rofi, &st, 0) < 0) {}
    if (catcher > 0) waitpid(catcher, NULL, 0);   // keep the two catch calls in order
    pid_t uncatch = catch_popup("");
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && rl > 0;
    if (!ok) { waitpid(uncatch, NULL, 0); return 0; }

    char *nl = strchr(res, '\n');
    if (nl) *nl = 0;
    char *tab = strchr(res, '\t');
    char *typed = tab ? (*tab = 0, trim(tab + 1)) : "";
    char *idx = res;
    int remove = 0;
    if (strcmp(typed, "rm") == 0 || strncmp(typed, "rm ", 3) == 0) {
        remove = 1;
        typed = trim(typed + 2);
        if (!*typed) { waitpid(uncatch, NULL, 0); return 0; }   // bare "rm": never guess which app
    }
    long pick = -1;
    if (all_digits(typed) && strlen(typed) < 9 && atol(typed) >= 1 && (size_t)atol(typed) <= n)
        pick = atol(typed) - 1;   // "12" + Enter = app number 12
    else if (all_digits(idx) && strlen(idx) < 9 && (size_t)atol(idx) < no)
        pick = order[atol(idx)];
    if (pick < 0) { waitpid(uncatch, NULL, 0); return 0; }
    App *a = &apps[pick];

    char name[1024];   // display name = search text minus " NUM rm"
    snprintf(name, sizeof name, "%s", a->search);
    for (int i = 0; i < 2; i++) { char *s = strrchr(name, ' '); if (s) *s = 0; }

    if (remove) {
        char title[1100], script[PATH_MAX];
        snprintf(title, sizeof title, "Remove %s", name);
        snprintf(script, sizeof script, "%s/scripts/remove-app.sh", ROOT);
        spawn((char *[]){"kitty", "--class", "hyprminimal-remove", "--title", title, script, a->file, name, NULL});
    } else {
        spawn((char *[]){"gio", "launch", a->file, NULL});
        mkdir(state, 0755);
        char tmp[PATH_MAX + 8];
        snprintf(tmp, sizeof tmp, "%s.tmp", recent_path);
        FILE *f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "%s\n", a->id);
            int kept = 1;
            for (int i = 0; i < nrid && kept < KEEP; i++)
                if (strcmp(recent_ids[i], a->id) != 0) { fprintf(f, "%s\n", recent_ids[i]); kept++; }
            if (fclose(f) == 0) rename(tmp, recent_path);
        }
    }
    waitpid(uncatch, NULL, 0);
    return 0;
}
