#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define MAX_HEADER_SIZE 8192
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

enum http_status { HTTP_OK, HTTP_INCOMPLETE, HTTP_BAD };
struct slice {
    const char *p;
    size_t n;
};
struct http_header {
    struct slice name, value;
};
struct http_request {
    struct slice method, target;
    int minor; // HTTP/1.x
    struct http_header headers[32];
    size_t nheaders;
    size_t head_len; // bytes consumed incl. blank line
};
enum http_status http_parse(const char *buf, size_t len, struct http_request *r);

/* X(number, NAME, reason). Add a row here and everything else follows. */
#define HTTP_STATUS_LIST(X)                                                                                            \
    X(200, OK, "OK")                                                                                                   \
    X(201, CREATED, "Created")                                                                                         \
    X(204, NO_CONTENT, "No Content")                                                                                   \
    X(301, MOVED_PERMANENTLY, "Moved Permanently")                                                                     \
    X(302, FOUND, "Found")                                                                                             \
    X(304, NOT_MODIFIED, "Not Modified")                                                                               \
    X(400, BAD_REQUEST, "Bad Request")                                                                                 \
    X(401, UNAUTHORIZED, "Unauthorized")                                                                               \
    X(403, FORBIDDEN, "Forbidden")                                                                                     \
    X(404, NOT_FOUND, "Not Found")                                                                                     \
    X(405, METHOD_NOT_ALLOWED, "Method Not Allowed")                                                                   \
    X(413, CONTENT_TOO_LARGE, "Content Too Large")                                                                     \
    X(431, HEADER_FIELDS_TOO_LARGE, "Request Header Fields Too Large")                                                 \
    X(500, INTERNAL_SERVER_ERROR, "Internal Server Error")                                                             \
    X(501, NOT_IMPLEMENTED, "Not Implemented")                                                                         \
    X(503, SERVICE_UNAVAILABLE, "Service Unavailable")

enum http_code {
#define X(num, name, reason) HTTP_##num##_##name = num,
    HTTP_STATUS_LIST(X)
#undef X
};
const char *http_code_reason(enum http_code code);
struct http_response {
    enum http_code status_code; /* 0 = not set yet */
    const char *reason;         /* points to static storage */

    struct http_header headers[32];
    size_t nheaders;

    char *body;
    size_t body_len;

    char buffer[512 * 1024];
    size_t buffer_len;
};

const char *http_code_reason(enum http_code code);

struct http_response {
    enum http_code status_code; /* 0 = not set yet */
    const char *reason;

    struct http_header headers[32];
    size_t nheaders;

    char *body;
    size_t body_len;

    char buffer[512 * 1024];
    size_t buffer_len;
};


struct http_response *http_response_new(void);
void http_response_free(struct http_response *res);
int http_response_status(struct http_response *res, enum http_code code);
int http_response_header(struct http_response *res, const char *name, const char *value);
int http_response_body(struct http_response *res, const void *data, size_t len);
int http_response_build(struct http_response *res, const char **out, size_t *out_len);

#endif