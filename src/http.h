#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define MAX_HEADER_SIZE 8192
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

enum http_status { HTTP_OK, HTTP_INCOMPLETE, HTTP_BAD };
struct slice { const char *p; size_t n; };
struct http_header { struct slice name, value; };
struct http_request {
    struct slice method, target;
    int minor;                       // HTTP/1.x
    struct http_header headers[32];
    size_t nheaders;
    size_t head_len;                 // bytes consumed incl. blank line
};
enum http_status http_parse(const char *buf, size_t len, struct http_request *r);

#endif