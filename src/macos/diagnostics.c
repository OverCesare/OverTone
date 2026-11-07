#include "diagnostics.h"
#include "utf8.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

struct ot_diagnostics {
    int socket, wake[2];
    pthread_t thread;
    char directory[64], path[108], manifest[4096];
    char *(*request)(void *, const char *);
    void *context;
};
static int64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static bool read_request(int fd, int wake, char *request) {
    size_t size = 0;
    int64_t started = milliseconds();
    if (started < 0)
        return false;
    int64_t deadline = started + 200;
    /* One absolute deadline, including fragmented input, and a wakeable shutdown. */
    while (size < OT_DIAGNOSTICS_REQUEST_BYTES) {
        int64_t now = milliseconds(), remaining = deadline - now;
        if (now < 0 || remaining <= 0)
            return false;
        struct pollfd watched[] = {{.fd = fd, .events = POLLIN}, {.fd = wake, .events = POLLIN}};
        int ready = poll(watched, 2, (int)remaining);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || watched[1].revents || !(watched[0].revents & POLLIN))
            return false;
        ssize_t n = read(fd, request + size, OT_DIAGNOSTICS_REQUEST_BYTES - size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        for (size_t i = size; i < size + (size_t)n; i++) {
            if (request[i] == '\n') {
                if (!i || i + 1 != size + (size_t)n)
                    return false;
                request[i] = 0;
                return ot_utf8_valid(request, i);
            }
            if ((unsigned char)request[i] < 32 || (unsigned char)request[i] == 127)
                return false;
        }
        size += (size_t)n;
    }
    return false;
}
static int connect_socket(const char *path, int fd, int bind_socket) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof address.sun_path)
        return -1;
    strcpy(address.sun_path, path);
    return bind_socket ? bind(fd, (struct sockaddr *)&address, sizeof address)
                       : connect(fd, (struct sockaddr *)&address, sizeof address);
}
static void *serve(void *pointer) {
    ot_diagnostics *s = pointer;
    for (;;) {
        struct pollfd watched[] = {{.fd = s->socket, .events = POLLIN},
                                   {.fd = s->wake[0], .events = POLLIN}};
        int ready = poll(watched, 2, -1);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || watched[1].revents || (watched[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
            break;
        if (!(watched[0].revents & POLLIN))
            continue;
        int client = accept(s->socket, NULL, NULL);
        if (client < 0)
            continue;
        uid_t user;
        gid_t group;
        if (!fcntl(client, F_SETFD, FD_CLOEXEC) &&
            getpeereid(client, &user, &group) == 0 && user == getuid()) {
            int yes = 1;
            setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
            struct timeval timeout = {.tv_usec = 200000};
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
            char request[OT_DIAGNOSTICS_REQUEST_BYTES];
            char *json = read_request(client, s->wake[0], request) ? s->request(s->context, request) : NULL;
            if (json) {
                size_t length = strnlen(json, OT_DIAGNOSTICS_MAX_BYTES + 1), offset = 0;
                while (length <= OT_DIAGNOSTICS_MAX_BYTES && offset < length) {
                    ssize_t n = write(client, json + offset, length - offset);
                    if (n < 0 && errno == EINTR)
                        continue;
                    if (n <= 0)
                        break;
                    offset += (size_t)n;
                }
                free(json);
            }
        }
        close(client);
    }
    return NULL;
}
ot_diagnostics *ot_diagnostics_open(const char *profile, char *(*request)(void *, const char *), void *context) {
    ot_diagnostics *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->socket = s->wake[0] = s->wake[1] = -1;
    s->request = request;
    s->context = context;
    strcpy(s->directory, "/private/tmp/overtone-diag-XXXXXX");
    if (!mkdtemp(s->directory))
        goto fail;
    snprintf(s->path, sizeof s->path, "%s/socket", s->directory);
    if (snprintf(s->manifest, sizeof s->manifest, "%s/socket.path", profile) >= (int)sizeof s->manifest)
        goto fail;
    s->socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->socket < 0 || connect_socket(s->path, s->socket, 1) || chmod(s->path, 0600) ||
        listen(s->socket, 4) || pipe(s->wake) || fcntl(s->socket, F_SETFD, FD_CLOEXEC) ||
        fcntl(s->wake[0], F_SETFD, FD_CLOEXEC) || fcntl(s->wake[1], F_SETFD, FD_CLOEXEC))
        goto fail;
    int fd = open(s->manifest, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        goto fail;
    size_t size = strlen(s->path);
    bool ok = write(fd, s->path, size) == (ssize_t)size;
    close(fd);
    if (!ok || pthread_create(&s->thread, NULL, serve, s))
        goto fail;
    return s;
fail:
    for (unsigned i = 0; i < 2; i++)
        if (s->wake[i] >= 0)
            close(s->wake[i]);
    if (s->socket >= 0)
        close(s->socket);
    if (*s->path)
        unlink(s->path);
    if (*s->manifest)
        unlink(s->manifest);
    if (*s->directory)
        rmdir(s->directory);
    free(s);
    return NULL;
}
void ot_diagnostics_close(ot_diagnostics *s) {
    if (!s)
        return;
    /* Closing our pipe wakes the blocked listener without periodic idle polling. */
    close(s->wake[1]);
    pthread_join(s->thread, NULL);
    close(s->wake[0]);
    close(s->socket);
    unlink(s->path);
    unlink(s->manifest);
    rmdir(s->directory);
    free(s);
}
static int exchange(const char *profile, const char *request) {
    char manifest[4096], path[108] = {0};
    if (snprintf(manifest, sizeof manifest, "%s/socket.path", profile) >= (int)sizeof manifest)
        return 1;
    int file = open(manifest, O_RDONLY | O_NOFOLLOW);
    if (file < 0)
        return 1;
    ssize_t n = read(file, path, sizeof path);
    close(file);
    if (n <= 0 || n >= (ssize_t)sizeof path)
        return 1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
    struct timeval timeout = {.tv_sec = 3};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    uid_t user;
    gid_t group;
    bool ok = !connect_socket(path, fd, 0) && !getpeereid(fd, &user, &group) && user == getuid();
    size_t sent = 0, length = strlen(request);
    while (ok && sent < length) {
        n = write(fd, request + sent, length - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) ok = false;
        else sent += (size_t)n;
    }
    if (ok) ok = !shutdown(fd, SHUT_WR);
    char data[OT_DIAGNOSTICS_MAX_BYTES + 1];
    size_t size = 0;
    while (ok && (n = read(fd, data + size, sizeof data - size)) > 0) {
        size += (size_t)n;
        if (size == sizeof data) {
            ok = false;
            break;
        }
    }
    if (n < 0 || !size)
        ok = false;
    close(fd);
    if (ok)
        ok = fwrite(data, 1, size, stdout) == size;
    if (ok && !strncmp(request, "control ", 8)) {
        yyjson_doc *doc = yyjson_read(data, size, 0);
        ok = doc && yyjson_is_true(yyjson_obj_get(yyjson_doc_get_root(doc), "ok"));
        yyjson_doc_free(doc);
    }
    return ok ? 0 : 1;
}
int ot_inspect(const char *profile) {
    return exchange(profile, "inspect\n");
}
int ot_control(const char *profile, const char *command) {
    char request[OT_DIAGNOSTICS_REQUEST_BYTES];
    if (strpbrk(command, "\r\n") ||
        snprintf(request, sizeof request, "control %s\n", command) >= (int)sizeof request)
        return 2;
    return exchange(profile, request);
}
