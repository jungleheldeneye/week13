#include "router.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum { FILE_CHUNK_BYTES = 16 * 1024 };

const char *static_file_mime_type(const char *path)
{
    static const struct {
        const char *extension;
        const char *type;
    } types[] = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
        {".webp", "image/webp"},
        {".pdf", "application/pdf"}
    };
    const char *extension = strrchr(path, '.');
    const char *slash = strrchr(path, '/');

    if (extension != NULL && (slash == NULL || extension > slash)) {
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
            if (strcasecmp(extension, types[i].extension) == 0) {
                return types[i].type;
            }
        }
    }
    return "application/octet-stream";
}

static int file_error(int error)
{
    switch (error) {
    case EACCES:
    case EPERM:
    case ELOOP:
        return 403;
    case ENOENT:
    case ENOTDIR:
        return 404;
    case ENAMETOOLONG:
        return 414;
    default:
        return 500;
    }
}

void static_file_close(static_file *file)
{
    int saved_errno = errno;

    if (file->fd >= 0) close(file->fd);
    file->fd = -1;
    errno = saved_errno;
}

int static_file_open(int root_fd, const char *path, static_file *file)
{
    char relative[HTTP_MAX_URI_BYTES + 1];
    int directory_fd;
    int status = 0;
    char *component;
    size_t length = strlen(path);

    *file = (static_file){.fd = -1};
    if (length > HTTP_MAX_URI_BYTES) return 414;
    if (*path != '/' || path[1] == '\0') return 403;
    memcpy(relative, path + 1, length);
    directory_fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (directory_fd == -1) return file_error(errno);
    component = relative;

    for (;;) {
        char *next = strchr(component, '/');
        struct stat before;
        struct stat opened;
        int fd;
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;

        if (next != NULL) *next = '\0';
        if (*component == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0) {
            status = 403;
            break;
        }
        if (fstatat(directory_fd, component, &before, AT_SYMLINK_NOFOLLOW) == -1) {
            status = file_error(errno);
            break;
        }
        if (S_ISLNK(before.st_mode)) {
            status = 403;
            break;
        }
        if (next != NULL) {
            if (!S_ISDIR(before.st_mode)) {
                status = 404;
                break;
            }
            flags |= O_DIRECTORY;
        } else if (!S_ISREG(before.st_mode)) {
            status = 403;
            break;
        }

        /* 사전 검사 뒤 경로가 바뀌어도 심볼릭 링크를 따라가지 않는다. */
        fd = openat(directory_fd, component, flags);
        if (fd == -1) {
            status = file_error(errno);
            break;
        }
        if (fstat(fd, &opened) == -1) {
            status = file_error(errno);
            close(fd);
            break;
        }
        if (next == NULL) {
            if (!S_ISREG(opened.st_mode) || opened.st_size < 0) {
                status = 403;
                close(fd);
                break;
            }
            file->fd = fd;
            file->length = opened.st_size;
            file->content_type = static_file_mime_type(path);
            break;
        }
        if (!S_ISDIR(opened.st_mode)) {
            status = 404;
            close(fd);
            break;
        }
        close(directory_fd);
        directory_fd = fd;
        component = next + 1;
    }

    close(directory_fd);
    return status;
}

static ssize_t read_file(int fd, void *buffer, size_t capacity)
{
    ssize_t count;

    do {
        count = read(fd, buffer, capacity);
    } while (count == -1 && errno == EINTR);
    return count;
}

int static_file_serve(int client_fd, int root_fd, const char *path, bool *keep_alive)
{
    static_file file;
    unsigned char chunk[FILE_CHUNK_BYTES];
    off_t remaining;
    ssize_t count = 0;
    int result;
    int status = static_file_open(root_fd, path, &file);
    http_response response;

    if (status != 0) {
        *keep_alive = false;
        return http_send_error(client_fd, status);
    }
    remaining = file.length;
    /* 첫 읽기에 실패하면 200을 보내기 전에 500으로 응답한다. */
    if (remaining > 0) {
        size_t amount = remaining < (off_t)sizeof(chunk) ? (size_t)remaining : sizeof(chunk);

        count = read_file(file.fd, chunk, amount);
        if (count <= 0) {
            static_file_close(&file);
            *keep_alive = false;
            return http_send_error(client_fd, 500);
        }
    }
    response = (http_response){.status = 200, .content_type = file.content_type,
        .content_length = file.length, .keep_alive = *keep_alive};
    result = http_send_headers(client_fd, &response);
    while (result == 0 && remaining > 0) {
        if (count <= 0) {
            /* 전송 시작 뒤 파일이 줄어들면 다른 응답을 덧붙이지 않는다. */
            errno = EIO;
            result = -1;
            break;
        }
        result = http_send_all(client_fd, chunk, (size_t)count);
        remaining -= count;
        if (result == 0 && remaining > 0) {
            size_t amount = remaining < (off_t)sizeof(chunk) ? (size_t)remaining : sizeof(chunk);

            count = read_file(file.fd, chunk, amount);
        }
    }
    static_file_close(&file);
    return result;
}
