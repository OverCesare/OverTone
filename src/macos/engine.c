#include "engine.h"
#include "process.h"
#include <errno.h>
#include <poll.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
void ot_engine_init(ot_engine *e) {
    memset(e, 0, sizeof *e);
    e->pid = -1;
    e->pipe = -1;
    e->control = -1;
}
bool ot_engine_find(const char *name, char out[4096]) {
    if (strchr(name, '/'))
        return realpath(name, out) && ot_process_executable(out);
    const char *path = getenv("PATH");
    char search[16384];
    snprintf(search, sizeof search, "%s:/opt/homebrew/bin:/usr/local/bin:/usr/bin", path ? path : "");
    char *save = NULL;
    for (char *part = strtok_r(search, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
        char candidate[4096];
        if (*part != '/' ||
            snprintf(candidate, sizeof candidate, "%s/%s", part, name) >= (int)sizeof candidate)
            continue;
        if (realpath(candidate, out) && ot_process_executable(out))
            return true;
    }
    if (!strcmp(name, "ollama")) {
        const char *home = getenv("HOME");
        char candidate[4096];
        if (realpath("/Applications/Ollama.app/Contents/Resources/ollama", out) && ot_process_executable(out))
            return true;
        if (home && snprintf(candidate, sizeof candidate, "%s/Applications/Ollama.app/Contents/Resources/ollama", home) < (int)sizeof candidate &&
            realpath(candidate, out) && ot_process_executable(out)) return true;
    }
    *out = 0;
    return false;
}
static unsigned port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t n = sizeof address;
    bool ok = !bind(fd, (struct sockaddr *)&address, n) && !getsockname(fd, (struct sockaddr *)&address, &n);
    close(fd);
    return ok ? ntohs(address.sin_port) : 0;
}
static bool help_supports(const char *file, const char *needle, const char *second, double seconds) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-qual"
    char *arguments[] = {(char *)file, "--help", NULL};
#pragma clang diagnostic pop
    char *output = ot_process_capture(file, arguments, seconds, NULL, false, -1);
    bool supported = output && strstr(output, needle) && (!second || strstr(output, second));
    free(output);
    return supported;
}
static uint64_t fit_target(const ot_preferences *p, const ot_resources *r) {
    uint64_t budget = r->memory_total / 100 * p->memory_percent;
    uint64_t margin = r->gpu_recommended > budget ? r->gpu_recommended - budget : 0;
    return (margin < UINT64_C(1073741824) ? UINT64_C(1073741824) : margin) / 1048576;
}
bool ot_engine_memory_plan(const char *file, const char *model, const ot_preferences *p,
                           const ot_resources *r, const atomic_bool *cancel, ot_memory_plan *plan) {
    char tool[4096], resolved[4096], context[16], layers[16], threads[16], fit[32];
    if (!realpath(file, tool))
        return false;
    char *name = strrchr(tool, '/');
    if (!name || (size_t)(name - tool) + sizeof "/llama-fit-params" > sizeof tool)
        return false;
    strcpy(name, "/llama-fit-params");
    if (!realpath(tool, resolved))
        return false;
    snprintf(context, sizeof context, "%u", p->context);
    snprintf(layers, sizeof layers, "%d", p->gpu_layers < 0 ? (r->gpu ? 999 : 0) : p->gpu_layers);
    snprintf(threads, sizeof threads, "%u", ot_thread_budget(r, p));
    snprintf(fit, sizeof fit, "%llu", (unsigned long long)fit_target(p, r));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-qual"
    char *arguments[24] = {
        resolved, "-m",  (char *)model,  "-c",   context,       "-t", threads,       "-np", "1", "-b", "2048",
        "-ub", "512", "--flash-attn", "auto", "--fit-print", "on", "--log-jsonl",
        "--fit-target", fit, NULL};
    if (p->gpu_layers != -1 || !r->gpu) {
        size_t count = 0;
        while (arguments[count])
            count++;
        arguments[count++] = "-ngl";
        arguments[count++] = layers;
        arguments[count] = NULL;
    }
#pragma clang diagnostic pop
    char *output = ot_process_capture(resolved, arguments, 5, cancel, true, -1);
    bool ok = false;
    if (output) {
        char *save = NULL;
        for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
            if (*line == '{' && ot_memory_parse(line, strlen(line), plan))
                ok = true;
    }
    free(output);
    return ok;
}
bool ot_engine_memory(const ot_engine *e, uint64_t *bytes) {
    if (e->pid <= 0 || e->child <= 0 || e->stopping)
        return false;
    pid_t queue[64] = {e->child}, parents[64] = {e->pid};
    unsigned count = 1;
    uint64_t total = 0;
    for (unsigned i = 0; i < count; i++) {
        struct proc_bsdinfo info;
        struct rusage_info_v4 usage;
        if (proc_pidinfo(queue[i], PROC_PIDTBSDINFO, 0, &info, sizeof info) != sizeof info ||
            info.pbi_ppid != (uint32_t)parents[i] || info.pbi_uid != getuid() ||
            proc_pid_rusage(queue[i], RUSAGE_INFO_V4, (rusage_info_t *)&usage) ||
            usage.ri_phys_footprint > UINT64_MAX - total)
            return false;
        total += usage.ri_phys_footprint;
        pid_t children[64];
        errno = 0;
        int listed = proc_listchildpids(queue[i], children, sizeof children);
        /* Unlike proc_listpids, this wrapper returns a PID count, not a byte count. */
        if (listed < 0 || errno || listed >= 64)
            return false;
        unsigned found = (unsigned)listed;
        if (found > 64 - count)
            return false;
        for (unsigned j = 0; j < found; j++) {
            if (children[j] <= 0)
                return false;
            for (unsigned k = 0; k < count; k++)
                if (children[j] == queue[k])
                    return false;
            queue[count] = children[j];
            parents[count++] = queue[i];
        }
    }
    *bytes = total;
    return true;
}
bool ot_engine_sleep_supported(const char *file) {
    return help_supports(file, "--sleep-idle-seconds", NULL, 2);
}
bool ot_engine_validate(const char *file, bool ollama) {
    /* Validation runs on the library worker. A newly extracted binary can need
     * cold dyld/Metal initialization; a short capability probe's budget is insufficient. */
    return help_supports(file, ollama ? "ollama" : "--model", ollama ? "serve" : "--ctx-size", 8);
}
bool ot_engine_start(ot_engine *e, const char *file, const char *profile, const char *model, bool ollama,
                     bool idle_sleep, const char *store, const ot_preferences *p, const ot_resources *r) {
    if (e->pid > 0 || !ot_process_executable(file))
        return false;
    unsigned selected = port();
    if (!selected)
        return false;
    char threads[16], context[16], layers[16], listen[16], idle[16], home[4096], host[64], models[4096],
        tmp[4096], fit[96];
    snprintf(threads, sizeof threads, "%u", ot_thread_budget(r, p));
    snprintf(context, sizeof context, "%u", p->context);
    snprintf(layers, sizeof layers, "%d", p->gpu_layers < 0 ? (r->gpu ? 999 : 0) : p->gpu_layers);
    snprintf(listen, sizeof listen, "%u", selected);
    int idle_seconds = !strcmp(p->keep_alive, "-1") ? -1 : atoi(p->keep_alive) * 60;
    /* llama.cpp rejects zero; one idle second is its minimum automatic release period. */
    if (!idle_seconds)
        idle_seconds = 1;
    snprintf(idle, sizeof idle, "%d", idle_seconds);
    const char *user_home = getenv("HOME"), *model_root = store ? store : getenv("OLLAMA_MODELS"),
               *temporary = getenv("TMPDIR");
    if (!user_home)
        return false;
    if (snprintf(home, sizeof home, "HOME=%s", user_home) >= (int)sizeof home ||
        snprintf(tmp, sizeof tmp, "TMPDIR=%s", temporary ? temporary : "/private/tmp") >= (int)sizeof tmp)
        return false;
    snprintf(host, sizeof host, "OLLAMA_HOST=127.0.0.1:%u", selected);
    if (snprintf(models, sizeof models, "OLLAMA_MODELS=%s", model_root ? model_root : "") >=
        (int)sizeof models)
        return false;
    snprintf(fit, sizeof fit, "LLAMA_ARG_FIT_TARGET=%llu", (unsigned long long)fit_target(p, r));
    char *environment[] = {home,
                           tmp,
                           "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
                           "LANG=en_US.UTF-8",
                           host,
                           fit,
                           "OLLAMA_MAX_LOADED_MODELS=1",
                           "OLLAMA_NUM_PARALLEL=1",
                           "OLLAMA_NO_CLOUD=true",
                           model_root ? models : NULL,
                           NULL,
                           NULL};
    if (store) {
        size_t count=0;while (environment[count]) count++;
        environment[count]="OLLAMA_NOPRUNE=true";environment[count+1]=NULL;
    }
    if (!ollama) {
        /* Core INFO is level 4 in recent llama.cpp; level 5 also enables prompt debugging.
         * An unsupported environment variable is ignored by older executables. */
        size_t count = 0;
        while (environment[count])
            count++;
        environment[count] = "LLAMA_ARG_LOG_VERBOSITY=4";
    }
    /* posix_spawn copies argv and never modifies its strings; its POSIX signature lacks const. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-qual"
    char *llama[20] = {(char *)file, "-m",    (char *)model, "--host", "127.0.0.1", "--port", listen,
                       "-c",         context, "-t",          threads,  "-np",       "1",      NULL};
    size_t llama_count = 0;
    while (llama[llama_count])
        llama_count++;
    /* Automatic delegates placement to this engine/version instead of forcing 999
     * layers. Explicit all/custom/CPU choices still send an explicit offload limit. */
    if (p->gpu_layers != -1 || !r->gpu) {
        llama[llama_count++] = "-ngl";
        llama[llama_count++] = layers;
    }
    if (idle_sleep) {
        llama[llama_count++] = "--sleep-idle-seconds";
        llama[llama_count++] = idle;
    }
    llama[llama_count] = NULL;
    char *other[] = {(char *)file, "serve", NULL};
#pragma clang diagnostic pop
    char own_path[4096], resolved[4096];
    uint32_t path_size = sizeof own_path;
    if (_NSGetExecutablePath(own_path, &path_size) || !realpath(own_path, resolved))
        return false;
    char *arguments[32] = {resolved, "--supervise"};
    char **child_args = ollama ? other : llama;
    size_t arg = 0;
    while (child_args[arg] && arg < 28) {
        arguments[arg + 2] = child_args[arg];
        arg++;
    }
    if (child_args[arg])
        return false;
    arguments[arg + 2] = NULL;
    int pipes[2], control[2];
    if (pipe(pipes))
        return false;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, control)) {
        close(pipes[0]);
        close(pipes[1]);
        return false;
    }
    fcntl(control[0], F_SETFL, O_NONBLOCK);
    fcntl(control[0], F_SETFD, FD_CLOEXEC);
    fcntl(control[1], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions)) {
        close(pipes[0]);
        close(pipes[1]);
        close(control[0]);
        close(control[1]);
        return false;
    }
    bool ok = !posix_spawnattr_init(&attributes);
    if (ok) {
        ok = !posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) &&
             !posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0) &&
             !posix_spawn_file_actions_adddup2(&actions, pipes[1], 2) &&
             !posix_spawn_file_actions_adddup2(&actions, control[1], 3) &&
             !posix_spawn_file_actions_addchdir_np(&actions, profile) &&
             !posix_spawnattr_setpgroup(&attributes, 0) &&
             !posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
        pid_t child = -1;
        if (ok)
            ok = posix_spawn(&child, resolved, &actions, &attributes, arguments, environment) == 0;
        if (ok) {
            e->pid = child;
            e->child = -1;
            e->handshake_size = 0;
            e->control = control[0];
            e->port = selected;
            e->pipe = pipes[0];
            e->tail_size = 0;
            e->tail[0] = 0;
            e->stopping = e->killed = false;
            e->exit_code = 0;
            e->runtime = (ot_runtime_info){0};
            e->buffers = (ot_runtime_buffers){0};
            e->runtime_line_size = 0;
            e->runtime_line_overflow = false;
            e->ollama = ollama;
            e->runtime_revision++;
        }
        posix_spawnattr_destroy(&attributes);
    }
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    close(control[1]);
    if (!ok) {
        close(pipes[0]);
        close(control[0]);
    }
    return ok;
}
void ot_engine_stop(ot_engine *e) {
    if (e->pid <= 0 || e->stopping)
        return;
    e->stopping = true;
    e->deadline = now() + 6;
    if (e->control >= 0) {
        close(e->control);
        e->control = -1;
    }
    kill(-e->pid, SIGTERM);
}
void ot_engine_poll(ot_engine *e) {
    if (e->pid <= 0)
        return;
    if (e->control >= 0 && e->handshake_size < sizeof(pid_t)) {
        ssize_t got = read(e->control, e->handshake + e->handshake_size, sizeof(pid_t) - e->handshake_size);
        if (got > 0)
            e->handshake_size += (size_t)got;
        if (e->handshake_size == sizeof(pid_t))
            memcpy(&e->child, e->handshake, sizeof(pid_t));
    }
    char buffer[4096];
    size_t total = 0;
    ssize_t n;
    while (total < 65536 && (n = read(e->pipe, buffer, sizeof buffer)) > 0) {
        size_t count = (size_t)n;
        for (size_t i = 0; i < count; i++) {
            if (buffer[i] == '\n') {
                e->runtime_line[e->runtime_line_size] = 0;
                if (!e->runtime_line_overflow) {
                    bool geometry = ot_runtime_line(&e->runtime, e->runtime_line);
                    bool allocation = ot_runtime_buffer_line(&e->buffers, &e->runtime, e->runtime_line);
                    if (geometry || allocation)
                        e->runtime_revision++;
                }
                e->runtime_line_size = 0;
                e->runtime_line_overflow = false;
            } else if (e->runtime_line_size < 1024)
                e->runtime_line[e->runtime_line_size++] = buffer[i];
            else
                e->runtime_line_overflow = true;
        }
        size_t retain = e->tail_size;
        if (retain + count > 4096)
            retain = 4096 - count;
        memmove(e->tail, e->tail + e->tail_size - retain, retain);
        memcpy(e->tail + retain, buffer, count);
        e->tail_size = retain + count;
        e->tail[e->tail_size] = 0;
        total += count;
    }
    if (e->stopping && !e->killed && now() >= e->deadline) {
        kill(-e->pid, SIGKILL);
        e->killed = true;
    }
    int status = 0;
    pid_t found = waitpid(e->pid, &status, WNOHANG);
    if (found == e->pid || (found < 0 && errno == ECHILD)) {
        e->exit_code =
            WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        e->pid = -1;
        e->child = -1;
        e->stopping = e->killed = false;
        close(e->pipe);
        e->pipe = -1;
        if (e->control >= 0) {
            close(e->control);
            e->control = -1;
        }
    }
}
void ot_engine_close(ot_engine *e) {
    ot_engine_stop(e);
    double deadline = now() + 7;
    while (e->pid > 0 && now() < deadline) {
        ot_engine_poll(e);
        struct timespec delay = {.tv_nsec = 10000000};
        nanosleep(&delay, NULL);
    }
    if (e->pid > 0) {
        kill(-e->pid, SIGKILL);
        int status;
        while (waitpid(e->pid, &status, 0) < 0 && errno == EINTR) {
        }
        e->pid = -1;
        e->child = -1;
        e->stopping = e->killed = false;
        if (e->pipe >= 0)
            close(e->pipe);
        e->pipe = -1;
        if (e->control >= 0)
            close(e->control);
        e->control = -1;
    }
}

/* The supervisor owns the actual engine and watches a private socket held by OverTone.
 * A crash/forced quit closes that socket, so cleanup does not depend on app callbacks. */
static volatile sig_atomic_t supervisor_stop;
static void stopped(int signal_number) {
    (void)signal_number;
    supervisor_stop = 1;
}
int ot_engine_supervise(char *const arguments[]) {
    uid_t user;
    gid_t group;
    if (!arguments[0] || !ot_process_executable(arguments[0]) || getpeereid(3, &user, &group) || user != getuid())
        return 125;
    struct sigaction action = {0};
    action.sa_handler = stopped;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&files))
        return 125;
    if (posix_spawnattr_init(&attributes)) {
        posix_spawn_file_actions_destroy(&files);
        return 125;
    }
    bool ok = !posix_spawn_file_actions_addinherit_np(&files, 0) &&
              !posix_spawn_file_actions_addinherit_np(&files, 1) &&
              !posix_spawn_file_actions_addinherit_np(&files, 2) &&
              !posix_spawnattr_setpgroup(&attributes, 0) &&
              !posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT);
    extern char **environ;
    pid_t child = -1;
    if (ok)
        ok = posix_spawn(&child, arguments[0], &files, &attributes, arguments, environ) == 0;
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    if (!ok)
        return 126;
    int no_pipe = 1;
    setsockopt(3, SOL_SOCKET, SO_NOSIGPIPE, &no_pipe, sizeof no_pipe);
    if (write(3, &child, sizeof child) != (ssize_t)sizeof child)
        supervisor_stop = 1;
    int events = kqueue();
    struct kevent watches[2];
    EV_SET(&watches[0], 3, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, NULL);
    EV_SET(&watches[1], (uintptr_t)child, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (events < 0 || kevent(events, watches, 2, NULL, 0, NULL)) {
        if (events >= 0) close(events);
        events = -1;
        supervisor_stop = 1;
    }
    double deadline = 0;
    int status = 0;
    bool killed = false;
    for (;;) {
        siginfo_t ended = {0};
        int observed = waitid(P_PID, (id_t)child, &ended, WEXITED | WNOHANG | WNOWAIT);
        if (observed < 0 && errno != EINTR) {
            if (events >= 0) close(events);
            return 125;
        }
        if (!observed && ended.si_pid == child) {
            /* Keep the leader's PID reserved until its remaining process group is gone.
             * A server can exit before its workers, including workers ignoring SIGTERM. */
            kill(-child, SIGKILL);
            pid_t reaped;
            do {
                reaped = waitpid(child, &status, 0);
            } while (reaped < 0 && errno == EINTR);
            if (reaped != child) {
                if (events >= 0) close(events);
                return 125;
            }
            break;
        }
        if (supervisor_stop && deadline <= 0) {
            kill(-child, SIGTERM);
            deadline = now() + 3;
        }
        if (deadline > 0 && now() >= deadline && !killed) {
            kill(-child, SIGKILL);
            killed = true;
        }
        if (events >= 0) {
            /* Native exit and parent-socket events require no periodic wakeups.
             * Only termination needs a deadline before escalating to SIGKILL. */
            double remaining = deadline - now();
            struct timespec timeout = {.tv_sec = remaining > 0 ? (time_t)remaining : 0,
                .tv_nsec = remaining > 0 ? (long)((remaining - (double)(time_t)remaining) * 1e9) : 0};
            struct kevent event;
            int count = kevent(events, NULL, 0, &event, 1, deadline > 0 && !killed ? &timeout : NULL);
            if (count > 0 && (event.filter == EVFILT_READ || (event.flags & EV_ERROR)))
                supervisor_stop = 1;
            else if (count < 0 && errno != EINTR) {
                close(events);
                events = -1;
                supervisor_stop = 1;
            }
        } else {
            /* Bounded cleanup if event registration fails, never normal idle work. */
            struct timespec delay = {.tv_nsec = 50000000};
            nanosleep(&delay, NULL);
        }
    }
    if (events >= 0) close(events);
    close(3);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

bool ot_engine_listening(const ot_engine *e) {
    if (e->pid <= 0 || e->child <= 0 || e->stopping)
        return false;
    struct proc_bsdinfo process;
    if (proc_pidinfo(e->child, PROC_PIDTBSDINFO, 0, &process, sizeof process) != (int)sizeof process ||
        process.pbi_ppid != (uint32_t)e->pid)
        return false;
    int size = proc_pidinfo(e->child, PROC_PIDLISTFDS, 0, NULL, 0);
    if (size <= 0 || size > 1024 * 1024)
        return false;
    struct proc_fdinfo *files = malloc((size_t)size);
    if (!files)
        return false;
    int count = proc_pidinfo(e->child, PROC_PIDLISTFDS, 0, files, size) / (int)sizeof *files;
    bool found = false;
    for (int i = 0; i < count && !found; i++)
        if (files[i].proc_fdtype == PROX_FDTYPE_SOCKET) {
            struct socket_fdinfo socket;
            if (proc_pidfdinfo(e->child, files[i].proc_fd, PROC_PIDFDSOCKETINFO, &socket, sizeof socket) ==
                    (int)sizeof socket &&
                socket.psi.soi_kind == SOCKINFO_TCP && socket.psi.soi_family == AF_INET &&
                ntohs((uint16_t)socket.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport) == e->port)
                found = true;
        }
    free(files);
    return found;
}
