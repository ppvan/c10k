#include "http.h"
#include <string.h>

#define MAX_HEADER_SIZE 8192
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Pointer-based finders: return nullptr if not found. Working with [p, end)
 * instead of indexes removes the ssize_t/size_t casts. */
static const char *find_byte(const char *p, const char *end, char c) {
    if (p >= end)
        return nullptr;
    return memchr(p, c, (size_t)(end - p));
}

static const char *find_crlf(const char *p, const char *end) {
    for (const char *cr = find_byte(p, end, '\r'); cr != nullptr && cr + 1 < end; cr = find_byte(cr + 1, end, '\r')) {
        if (cr[1] == '\n')
            return cr;
    }
    return nullptr;
}

static bool is_ows(char c) { return c == ' ' || c == '\t'; }
enum http_status http_request_line_parse(const char *buf, size_t len, struct http_request *r, size_t *cursor) {
    const char *end = buf + len;

    const char *eol = find_crlf(buf, end);
    if (eol == NULL)
        return HTTP_INCOMPLETE; /* line not fully received yet */

    /* From here the line is complete, so any defect is final. */
    const char *sp1 = find_byte(buf, eol, ' ');
    if (sp1 == NULL || sp1 == buf)
        return HTTP_BAD;

    const char *target = sp1 + 1;
    const char *sp2 = find_byte(target, eol, ' ');
    if (sp2 == NULL || sp2 == target)
        return HTTP_BAD;

    const char *ver = sp2 + 1;
    if (eol - ver != 8 || memcmp(ver, "HTTP/1.", 7) != 0 || (ver[7] != '0' && ver[7] != '1'))
        return HTTP_BAD;

    r->method = (struct slice){.p = buf, .n = (size_t)(sp1 - buf)};
    r->target = (struct slice){.p = target, .n = (size_t)(sp2 - target)};
    r->minor = ver[7] - '0';

    *cursor = (size_t)(eol + 2 - buf);
    return HTTP_OK;
}

enum http_status http_headers_parse(const char *buf, size_t len, struct http_request *r, size_t *consumed) {
    const char *p = buf;
    const char *end = buf + len;

    r->nheaders = 0;

    for (;;) {
        const char *eol = find_crlf(p, end);
        if (eol == NULL)
            return HTTP_INCOMPLETE; /* no blank line yet, keep reading */

        if (eol == p) { /* blank line: end of headers */
            *consumed = (size_t)(eol + 2 - buf);
            return HTTP_OK;
        }

        if (r->nheaders >= ARRAY_LEN(r->headers))
            return HTTP_BAD; /* more than 32 headers */

        const char *colon = find_byte(p, eol, ':');
        if (colon == NULL || colon == p || is_ows(colon[-1]))
            return HTTP_BAD;

        const char *vbeg = colon + 1;
        const char *vend = eol;
        while (vbeg < vend && is_ows(*vbeg))
            vbeg++;
        while (vend > vbeg && is_ows(vend[-1]))
            vend--;

        r->headers[r->nheaders++] = (struct http_header){
            .name = {.p = p, .n = (size_t)(colon - p)},
            .value = {.p = vbeg, .n = (size_t)(vend - vbeg)},
        };

        p = eol + 2;
    }
}

enum http_status http_parse(const char *buf, size_t len, struct http_request *r) {
    memset(r, 0, sizeof *r);

    size_t line_len = 0;
    enum http_status s = http_request_line_parse(buf, len, r, &line_len);
    if (s != HTTP_OK)
        return s;

    size_t headers_len = 0;
    s = http_headers_parse(buf + line_len, len - line_len, r, &headers_len);
    if (s != HTTP_OK)
        return s;

    r->head_len = line_len + headers_len;
    return HTTP_OK;
}