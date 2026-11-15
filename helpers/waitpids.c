// waitpids TIMEOUT_MS PID...: sleep until one of the PIDs exits (any process, not only children) or the
// timeout passes (-1 = never). Lets the watchdog in bin/hyprminimal block on the kernel instead of
// waking every 3 s to scan /proc: zero wakeups while everything runs.
// Exit status: 0 = a process exited (or was already gone), 1 = timeout, 2 = usage/error.
// Built by bin/hyprminimal into .generated/bin/waitpids.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: waitpids TIMEOUT_MS PID...\n"); return 2; }
    char *end;
    long tl = strtol(argv[1], &end, 10);
    if (*end || end == argv[1] || tl < -1) { fprintf(stderr, "waitpids: bad timeout %s\n", argv[1]); return 2; }
    int timeout = (int)tl;
    int n = argc - 2;
    struct pollfd *fds = calloc(n > 0 ? n : 1, sizeof *fds);
    if (!fds) return 2;
    for (int i = 0; i < n; i++) {
        char *e;
        long pid = strtol(argv[i + 2], &e, 10);
        if (*e || pid <= 0) { fprintf(stderr, "waitpids: bad pid %s\n", argv[i + 2]); return 2; }
        int fd = (int)syscall(SYS_pidfd_open, (pid_t)pid, 0);
        if (fd < 0) {
            if (errno == ESRCH) return 0;     // already gone
            perror("pidfd_open");
            return 2;
        }
        fds[i].fd = fd;
        fds[i].events = POLLIN;               // readable = the process exited
    }
    for (;;) {
        int r = poll(fds, n, timeout);
        if (r > 0) return 0;
        if (r == 0) return 1;
        if (errno != EINTR) { perror("poll"); return 2; }
    }
}
