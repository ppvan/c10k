#include "http.h"
#include <errno.h> // IWYU pragma: keep
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

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
    if (eol == nullptr)
        return HTTP_INCOMPLETE; /* line not fully received yet */

    /* From here the line is complete, so any defect is final. */
    const char *sp1 = find_byte(buf, eol, ' ');
    if (sp1 == nullptr || sp1 == buf)
        return HTTP_BAD;

    const char *target = sp1 + 1;
    const char *sp2 = find_byte(target, eol, ' ');
    if (sp2 == nullptr || sp2 == target)
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
        if (eol == nullptr)
            return HTTP_INCOMPLETE; /* no blank line yet, keep reading */

        if (eol == p) { /* blank line: end of headers */
            *consumed = (size_t)(eol + 2 - buf);
            return HTTP_OK;
        }

        if (r->nheaders >= ARRAY_LEN(r->headers))
            return HTTP_BAD; /* more than 32 headers */

        const char *colon = find_byte(p, eol, ':');
        if (colon == nullptr || colon == p || is_ows(colon[-1]))
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

/* Layout of res->buffer:
 *
 *   [ strings ... ->            free            <- head | body ]
 *     0       buffer_len                       body_start    end
 *
 * Strings grow from the front, the body sits at the very end, and build()
 * writes the head into the free space directly before the body. */

static size_t free_space(const struct http_response *res) {
    return sizeof res->buffer - res->buffer_len - res->body_len;
}

/* Header names: visible ASCII, no ':'. */
static bool valid_name(const char *s, size_t n) {
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c >= 0x7f || c == ':')
            return false;
    }
    return true;
}

/* Values and reasons: no CR, LF or other control chars (tab is fine).
 * This is what prevents header injection / response splitting. */
static bool valid_value(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return false;
    }
    return true;
}

/* Copies s[0..n) into the arena as a NUL-terminated string. The caller has
 * already checked free_space(). */
static char *arena_put(struct http_response *res, const char *s, size_t n) {
    char *dst = res->buffer + res->buffer_len;
    memcpy(dst, s, n);
    dst[n] = '\0';
    res->buffer_len += n + 1;
    return dst;
}

static char *put(char *dst, const void *src, size_t n) {
    memcpy(dst, src, n);
    return dst + n;
}

struct http_response *http_response_new(void) {
    /* ~512 KiB: heap only. calloc of this size is mmap-backed, so the
     * zeroing is lazy and cheap. */
    return calloc(1, sizeof(struct http_response));
}

void http_response_free(struct http_response *res) { free(res); }

const char *http_code_reason(enum http_code code) {
    switch (code) {
#define X(num, name, reason)                                                                                           \
    case HTTP_##num##_##name:                                                                                          \
        return reason;
        HTTP_STATUS_LIST(X)
#undef X
    }
    return nullptr;
}

int http_response_status(struct http_response *res, enum http_code code) {
    const char *reason = http_code_reason(code);
    if (reason == NULL)
        return -EINVAL;

    res->status_code = code;
    res->reason = reason;
    return 0;
}

int http_response_header(struct http_response *res, const char *name, const char *value) {
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);

    if (!valid_name(name, nlen) || !valid_value(value, vlen))
        return -EINVAL;

    /* Framing belongs to the builder. */
    if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Transfer-Encoding") == 0)
        return -EINVAL;

    if (res->nheaders >= ARRAY_LEN(res->headers))
        return -ENOBUFS;
    if (nlen + vlen + 2 > free_space(res))
        return -ENOBUFS;

    const char *n = arena_put(res, name, nlen);
    const char *v = arena_put(res, value, vlen);

    res->headers[res->nheaders++] = (struct http_header){
        .name = {.p = n, .n = nlen},
        .value = {.p = v, .n = vlen},
    };
    return 0;
}

int http_response_body(struct http_response *res, const void *data, size_t len) {
    if (len > 0 && data == nullptr)
        return -EINVAL;

    /* A previous body is being replaced, so only strings count here. */
    if (len > sizeof res->buffer - res->buffer_len)
        return -ENOBUFS;

    char *dst = res->buffer + sizeof res->buffer - len;
    memmove(dst, data, len); /* memmove: data may point into the old body */

    res->body = dst;
    res->body_len = len;
    return 0;
}

int http_response_build(struct http_response *res, const char **out, size_t *out_len) {
    if (res->status_code == 0) {
        int rc = http_response_status(res, HTTP_200_OK);
        if (rc < 0)
            return rc;
    }

    char cl[24];
    int cl_digits = snprintf(cl, sizeof cl, "%zu", res->body_len);

    /* "HTTP/1.1 " + 3-digit code + " " + reason + CRLF */
    size_t reason_len = strlen(res->reason);
    size_t head_len = 9 + 3 + 1 + reason_len + 2;

    for (size_t i = 0; i < res->nheaders; i++)
        head_len += res->headers[i].name.n + 2 + res->headers[i].value.n + 2;

    head_len += (sizeof "Content-Length: " - 1) + (size_t)cl_digits + 2; /* Content-Length */
    head_len += 2;                                                       /* blank line */

    char *body_start = res->buffer + sizeof res->buffer - res->body_len;
    if (head_len > (size_t)(body_start - res->buffer) - res->buffer_len)
        return -ENOBUFS;

    char *start = body_start - head_len;
    char *d = start;

    d = put(d, "HTTP/1.1 ", 9);
    *d++ = (char)('0' + res->status_code / 100);
    *d++ = (char)('0' + res->status_code / 10 % 10);
    *d++ = (char)('0' + res->status_code % 10);
    *d++ = ' ';
    d = put(d, res->reason, reason_len);
    d = put(d, "\r\n", 2);

    for (size_t i = 0; i < res->nheaders; i++) {
        const struct http_header *h = &res->headers[i];
        d = put(d, h->name.p, h->name.n);
        d = put(d, ": ", 2);
        d = put(d, h->value.p, h->value.n);
        d = put(d, "\r\n", 2);
    }

    d = put(d, "Content-Length: ", sizeof "Content-Length: " - 1);
    d = put(d, cl, (size_t)cl_digits);
    d = put(d, "\r\n\r\n", 4);

    *out = start;
    *out_len = head_len + res->body_len;
    return 0;
}

bool http_name_is(struct slice s, const char *lit) {
    size_t n = strlen(lit);
    return s.n == n && strncasecmp(s.p, lit, n) == 0;
}

bool http_has_token(struct slice v, const char *tok) {
    size_t tl = strlen(tok);
    const char *p = v.p, *end = v.p + v.n;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
            p++;
        const char *q = p;
        while (q < end && *q != ',' && *q != ' ' && *q != '\t')
            q++;
        if ((size_t)(q - p) == tl && strncasecmp(p, tok, tl) == 0)
            return true;
        p = q;
    }
    return false;
}

bool http_wants_keep_alive(const struct http_request *r) {
    bool keep = r->minor >= 1;

    for (size_t i = 0; i < r->nheaders; i++) {
        const struct http_header *h = &r->headers[i];

        /* Request bodies are never read, so leftover body bytes would be
         * parsed as the next request. Close instead. */
        if (http_name_is(h->name, "Transfer-Encoding"))
            return false;
        if (http_name_is(h->name, "Content-Length") && !(h->value.n == 1 && h->value.p[0] == '0'))
            return false;

        if (http_name_is(h->name, "Connection")) {
            if (http_has_token(h->value, "close"))
                return false;
            if (http_has_token(h->value, "keep-alive"))
                keep = true;
        }
    }
    return keep;
}

struct slice http_target_normalize(struct slice t) {
    const char *q = memchr(t.p, '?', t.n);
    if (q != NULL)
        t.n = (size_t)(q - t.p);

    if (t.n == 1 && t.p[0] == '/')
        return (struct slice){"/index.html", 11};
    return t;
}

bool http_target_is_safe(struct slice t) {
    if (t.n == 0 || t.p[0] != '/' || memchr(t.p, '\0', t.n) != NULL)
        return false;

    for (size_t i = 0; i < t.n;) {
        size_t j = i + 1; /* skip the '/' */
        while (j < t.n && t.p[j] != '/')
            j++;
        if (j - (i + 1) == 2 && t.p[i + 1] == '.' && t.p[i + 2] == '.')
            return false;
        i = j;
    }
    return true;
}

static const struct {
    const char *ext, *type;
} mime_types[] = {
    {".html", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".txt", "text/plain; charset=utf-8"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".wasm", "application/wasm"},
    {".pdf", "application/pdf"},
};

const char *http_mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot != NULL) {
        for (size_t i = 0; i < ARRAY_LEN(mime_types); i++)
            if (strcasecmp(dot, mime_types[i].ext) == 0)
                return mime_types[i].type;
    }
    return "application/octet-stream";
}

int http_file_head(char *buf, size_t cap, const char *content_type, off_t size, bool keep) {
    int n = snprintf(buf, cap,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: %s\r\n\r\n",
                     content_type, (long long)size, keep ? "keep-alive" : "close");
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}