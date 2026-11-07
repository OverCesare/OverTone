#include "process.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
bool ot_process_executable(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st;
    unsigned char magic[4];
    bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0111) && read(fd, magic, 4) == 4;
    close(fd);
    return ok && (!memcmp(magic, "\xcf\xfa\xed\xfe", 4) || !memcmp(magic, "\xce\xfa\xed\xfe", 4) ||
                  !memcmp(magic, "\xca\xfe\xba\xbe", 4) || !memcmp(magic, "\xca\xfe\xba\xbf", 4));
}
char *ot_process_capture(const char *file, char *const arguments[], double seconds, const atomic_bool *cancel,
                         bool clean_environment, int input_fd) {
    if (!ot_process_executable(file))
        return NULL;
    int pipefd[2];
    if (pipe(pipefd))
        return NULL;
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions)) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    bool initialized = !posix_spawnattr_init(&attributes);
    bool ok = initialized && !posix_spawn_file_actions_addchdir_np(&actions, "/") &&
              (input_fd >= 0 ? !posix_spawn_file_actions_adddup2(&actions, input_fd, 0)
                             : !posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)) &&
              !posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1) &&
              !posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2) &&
              !posix_spawnattr_setpgroup(&attributes, 0) &&
              !posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
    extern char **environ;
    char home[4096], temporary[4096];
    const char *user_home = getenv("HOME"), *tmp = getenv("TMPDIR");
    char *environment[] = {home, temporary, "PATH=/opt/homebrew/bin:/usr/bin:/bin", "LANG=en_US.UTF-8", NULL};
    if (clean_environment)
        ok = ok && user_home && snprintf(home, sizeof home, "HOME=%s", user_home) < (int)sizeof home &&
             snprintf(temporary, sizeof temporary, "TMPDIR=%s", tmp ? tmp : "/private/tmp") <
                 (int)sizeof temporary;
    pid_t child = -1;
    if (ok)
        ok = !posix_spawn(&child, file, &actions, &attributes, arguments,
                          clean_environment ? environment : environ);
    if (initialized)
        posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    char *output = ok ? malloc(262145) : NULL;
    size_t size = 0;
    int status = 0;
    bool exited = false, reaped = false, eof = false;
    double deadline = now() + seconds;
    while (output && (!eof || !exited) && size < 262144 && now() < deadline &&
           (!cancel || !atomic_load(cancel))) {
        ssize_t n = eof ? -1 : read(pipefd[0], output + size, 262144 - size);
        if (n > 0)
            size += (size_t)n;
        else if (!n)
            eof = true;
        else if (!eof && errno != EAGAIN && errno != EINTR)
            break;
        if (!exited) {
            siginfo_t ended = {0};
            int observed = waitid(P_PID, (id_t)child, &ended, WEXITED | WNOHANG | WNOWAIT);
            if (observed < 0 && errno != EINTR)
                break;
            exited = !observed && ended.si_pid == child;
        }
        if (n < 0) {
            if (eof) {
                struct timespec delay = {.tv_nsec = 20000000};
                nanosleep(&delay, NULL);
            } else {
                struct pollfd watched = {.fd = pipefd[0], .events = POLLIN | POLLHUP};
                poll(&watched, 1, 20);
            }
        }
    }
    if (ok) {
        /* Temporary probes own the whole group, even descendants that closed stdout/stderr. */
        kill(-child, SIGKILL);
        pid_t found;
        do {
            found = waitpid(child, &status, 0);
        } while (found < 0 && errno == EINTR);
        reaped = found == child;
    }
    bool success = output && eof && exited && reaped && WIFEXITED(status) && !WEXITSTATUS(status) &&
                   (!cancel || !atomic_load(cancel));
    if (output) {
        output[size] = 0;
        if (!success) {
            free(output);
            output = NULL;
        }
    }
    close(pipefd[0]);
    return output;
}
