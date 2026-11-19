// Close the swaync control center (Super+S) and the bar popups (battery, Wi-Fi, ...) when the workspace or
// focused monitor changes (e.g. Super+2) or a window opens. They are layer surfaces, so Hyprland leaves
// them open (and on top of the new window) by itself.
// Blocks on Hyprland's event socket (no polling); started and stopped by bin/hyprminimal.
// Replaces scripts/workspace-watch.py (kept as the fallback when gcc is missing): ~0.5 MB instead of ~12 MB.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void hide_popups(void) {
    pid_t pid = fork();
    if (pid == 0) {
        // --auto-start=no: never D-Bus-activate the stock swaync
        execlp("busctl", "busctl", "--user", "--auto-start=no", "call", "org.erikreider.swaync.cc",
               "/org/erikreider/swaync/cc", "org.erikreider.swaync.cc", "SetVisibility", "b", "false",
               (char *)NULL);
        _exit(127);
    }
    pid = fork();
    if (pid == 0) {   // the bar popup, inside the resident hyprminimal-settings (no-op when none is open)
        execlp("gdbus", "gdbus", "call", "--session", "--dest", "dev.hyprminimal.Settings", "--object-path",
               "/dev/hyprminimal/Settings", "--method", "org.gtk.Actions.Activate", "panel-close", "[]", "{}",
               (char *)NULL);
        _exit(127);
    }
}

static int starts(const char *line, const char *prefix) { return strncmp(line, prefix, strlen(prefix)) == 0; }

static void watch(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return; }

    char buf[8192];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
        buf[len] = 0;
        int hit = 0;
        char *line = buf, *nl;
        while ((nl = memchr(line, '\n', len - (size_t)(line - buf)))) {
            *nl = 0;
            if (starts(line, "workspace>>") || starts(line, "focusedmon>>") || starts(line, "openwindow>>")) hit = 1;
            line = nl + 1;
        }
        len -= (size_t)(line - buf);
        memmove(buf, line, len);
        if (len == sizeof buf - 1) len = 0;   // absurdly long line: drop it
        if (hit) hide_popups();
    }
    close(fd);
}

int main(void) {
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!sig) { fputs("workspace-watch: not running inside Hyprland\n", stderr); return 1; }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];
    snprintf(path, sizeof path, "%s/hypr/%s/.socket2.sock", rt ? rt : "/tmp", sig);
    signal(SIGCHLD, SIG_IGN);   // reap busctl/gdbus children automatically
    while (access(path, F_OK) == 0) {   // Hyprland gone = socket gone: exit
        watch(path);
        sleep(1);
    }
    return 0;
}
