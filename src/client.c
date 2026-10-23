/*
 * c10k_bench.c - epoll-based load generator for a TCP echo server.
 *
 * Opens N concurrent connections (ramped at a fixed rate), and on each
 * connection sends a message, waits for the full echo, verifies every byte,
 * records the round-trip latency, then sends the next one (immediately, or
 * after a think time with -i). Prints live stats every second and a latency
 * summary at the end.
 *
 * Build: gcc -O2 -Wall -Wextra -pthread -o c10k_bench c10k_bench.c
 * Usage: ./c10k_bench [options] <address> <port>
 */
#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS   1024
#define HIST_BUCKETS 1024
#define WHEEL_SLOTS  4096 /* 1 ms per slot; power of two */
#define RX_BUF       65536

enum conn_state { ST_IDLE, ST_CONNECTING, ST_INFLIGHT, ST_THINKING, ST_CLOSED };

struct conn {
    int fd;
    enum conn_state state;
    bool want_out;          /* EPOLLOUT currently registered for a partial send */
    uint32_t wr_off, rd_off;
    uint64_t seq;
    uint64_t sent_ns;
    uint64_t due_ms;
    struct conn *next_timer;
    unsigned char *msg;     /* the message in flight; echo is compared to it */
};

struct config {
    struct sockaddr_storage addr;
    socklen_t addrlen;
    int conns, threads, duration_s, connect_rate;
    uint32_t msg_size, interval_ms;
};

/* Written only by the owning worker, read by main: relaxed atomics suffice. */
struct stats {
    _Atomic uint64_t active, connected, connect_fail, msgs, bytes, errors, mismatches;
    _Atomic int last_connect_errno, last_errno;
};

struct worker {
    pthread_t tid;
    int epfd;
    int nconns, opened;
    struct conn *conns;
    unsigned char *arena;
    struct conn *wheel[WHEEL_SLOTS];
    uint64_t wheel_ms;
    int wheel_count;
    uint32_t rng;
    uint64_t hist[HIST_BUCKETS];
    uint64_t lat_max_us;
    struct stats st;
    unsigned char rx[RX_BUF];
};

static struct config cfg = {
    .conns = 10000, .threads = 2, .duration_s = 30,
    .connect_rate = 5000, .msg_size = 64, .interval_ms = 0,
};
static atomic_bool stop_flag;
static volatile sig_atomic_t got_sigint;

/* ---------- small helpers ---------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ms(void) { return now_ns() / 1000000ull; }

static inline void stat_add(_Atomic uint64_t *c, uint64_t n) {
    atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + n,
                          memory_order_relaxed);
}

static inline uint64_t stat_get(_Atomic uint64_t *c) {
    return atomic_load_explicit(c, memory_order_relaxed);
}

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* Log-linear histogram: exact below 16 us, then 16 sub-buckets per power of
 * two (~6% resolution). Values are microseconds. */
static int hist_index(uint64_t us) {
    if (us < 16)
        return (int)us;
    int msb = 63 - __builtin_clzll(us);
    int sub = (int)((us >> (msb - 4)) & 15);
    int idx = 16 + (msb - 4) * 16 + sub;
    return idx < HIST_BUCKETS ? idx : HIST_BUCKETS - 1;
}

static uint64_t hist_value(int idx) {
    if (idx < 16)
        return (uint64_t)idx;
    int msb = (idx - 16) / 16 + 4;
    int sub = (idx - 16) % 16;
    return (uint64_t)(16 + sub) << (msb - 4);
}

static uint64_t hist_percentile(const uint64_t *h, uint64_t total, double p) {
    if (total == 0)
        return 0;
    uint64_t target = (uint64_t)(p * (double)total);
    if (target >= total)
        target = total - 1;
    uint64_t acc = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        acc += h[i];
        if (acc > target)
            return hist_value(i);
    }
    return hist_value(HIST_BUCKETS - 1);
}

static void set_events(struct worker *w, struct conn *c, uint32_t events) {
    struct epoll_event ev = {.events = events, .data.ptr = c};
    if (epoll_ctl(w->epfd, EPOLL_CTL_MOD, c->fd, &ev) < 0)
        err(EXIT_FAILURE, "epoll_ctl MOD");
}

/* ---------- connection lifecycle ---------- */

static void conn_close(struct worker *w, struct conn *c, int errnum) {
    if (c->state == ST_CLOSED)
        return;
    if (c->state == ST_CONNECTING) {
        stat_add(&w->st.connect_fail, 1);
        atomic_store_explicit(&w->st.last_connect_errno, errnum, memory_order_relaxed);
    } else {
        stat_add(&w->st.active, (uint64_t)-1);
        if (errnum >= 0) {
            stat_add(&w->st.errors, 1);
            atomic_store_explicit(&w->st.last_errno, errnum, memory_order_relaxed);
        }
    }
    close(c->fd); /* also removes it from the epoll set */
    c->fd = -1;
    c->state = ST_CLOSED;
}

static void timer_add(struct worker *w, struct conn *c, uint64_t due_ms) {
    c->due_ms = due_ms;
    uint64_t slot = due_ms > w->wheel_ms ? due_ms : w->wheel_ms + 1;
    if (slot - w->wheel_ms >= WHEEL_SLOTS) /* too far: park, re-check on pop */
        slot = w->wheel_ms + WHEEL_SLOTS - 1;
    struct conn **head = &w->wheel[slot & (WHEEL_SLOTS - 1)];
    c->next_timer = *head;
    *head = c;
    w->wheel_count++;
}

static void do_send(struct worker *w, struct conn *c) {
    while (c->wr_off < cfg.msg_size) {
        ssize_t n = send(c->fd, c->msg + c->wr_off, cfg.msg_size - c->wr_off,
                         MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!c->want_out) {
                    set_events(w, c, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
                    c->want_out = true;
                }
                return;
            }
            conn_close(w, c, errno);
            return;
        }
        c->wr_off += (uint32_t)n;
    }
    if (c->want_out) {
        set_events(w, c, EPOLLIN | EPOLLRDHUP);
        c->want_out = false;
    }
}

static void start_send(struct worker *w, struct conn *c) {
    /* Stamp the sequence number so each message differs from the last. */
    size_t stamp = cfg.msg_size < sizeof(c->seq) ? cfg.msg_size : sizeof(c->seq);
    memcpy(c->msg, &c->seq, stamp);
    c->wr_off = c->rd_off = 0;
    c->state = ST_INFLIGHT;
    c->sent_ns = now_ns();
    do_send(w, c);
}

static void schedule_next(struct worker *w, struct conn *c, uint64_t delay_ms) {
    if (delay_ms == 0) {
        start_send(w, c);
    } else {
        c->state = ST_THINKING;
        timer_add(w, c, now_ms() + delay_ms);
    }
}

static void timer_run(struct worker *w, uint64_t now) {
    while (w->wheel_ms < now) {
        w->wheel_ms++;
        struct conn **head = &w->wheel[w->wheel_ms & (WHEEL_SLOTS - 1)];
        struct conn *c = *head;
        *head = NULL;
        while (c) {
            struct conn *next = c->next_timer;
            c->next_timer = NULL;
            w->wheel_count--;
            if (c->state == ST_THINKING) {
                if (c->due_ms > w->wheel_ms)
                    timer_add(w, c, c->due_ms);
                else
                    start_send(w, c);
            }
            c = next;
        }
    }
}

static void conn_open(struct worker *w, struct conn *c) {
    int fd = socket(cfg.addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        stat_add(&w->st.connect_fail, 1);
        atomic_store_explicit(&w->st.last_connect_errno, errno, memory_order_relaxed);
        c->state = ST_CLOSED;
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    c->fd = fd;
    c->state = ST_CONNECTING;
    if (connect(fd, (struct sockaddr *)&cfg.addr, cfg.addrlen) < 0 &&
        errno != EINPROGRESS) {
        conn_close(w, c, errno);
        return;
    }
    struct epoll_event ev = {.events = EPOLLOUT, .data.ptr = c};
    if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        err(EXIT_FAILURE, "epoll_ctl ADD");
}

static void on_connected(struct worker *w, struct conn *c) {
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0)
        soerr = errno;
    if (soerr != 0) {
        conn_close(w, c, soerr);
        return;
    }
    stat_add(&w->st.connected, 1);
    stat_add(&w->st.active, 1);
    c->state = ST_THINKING; /* placeholder until the first send */
    set_events(w, c, EPOLLIN | EPOLLRDHUP);

    /* Spread the first sends over one interval to avoid a thundering herd. */
    uint64_t delay = cfg.interval_ms ? xorshift32(&w->rng) % cfg.interval_ms + 1 : 0;
    schedule_next(w, c, delay);
}

static void on_message_complete(struct worker *w, struct conn *c) {
    uint64_t lat_us = (now_ns() - c->sent_ns) / 1000;
    w->hist[hist_index(lat_us)]++;
    if (lat_us > w->lat_max_us)
        w->lat_max_us = lat_us;
    stat_add(&w->st.msgs, 1);
    c->seq++;
    schedule_next(w, c, cfg.interval_ms);
}

static void on_readable(struct worker *w, struct conn *c) {
    for (;;) {
        ssize_t n = recv(c->fd, w->rx, sizeof(w->rx), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            conn_close(w, c, errno);
            return;
        }
        if (n == 0) { /* server closed on us */
            conn_close(w, c, ECONNRESET);
            return;
        }
        /* The server can only echo what we already sent for this message. */
        if (c->state != ST_INFLIGHT || c->rd_off + (uint32_t)n > c->wr_off ||
            memcmp(w->rx, c->msg + c->rd_off, (size_t)n) != 0) {
            stat_add(&w->st.mismatches, 1);
            conn_close(w, c, EPROTO);
            return;
        }
        c->rd_off += (uint32_t)n;
        stat_add(&w->st.bytes, (uint64_t)n);
        if (c->rd_off == cfg.msg_size) {
            on_message_complete(w, c);
            return;
        }
    }
}

/* ---------- worker thread ---------- */

static void *worker_main(void *arg) {
    struct worker *w = arg;
    struct epoll_event evs[MAX_EVENTS];
    uint64_t start = now_ms();
    double rate_per_ms = cfg.connect_rate > 0
                             ? (double)cfg.connect_rate / cfg.threads / 1000.0
                             : 0.0;
    w->wheel_ms = start;

    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        uint64_t now = now_ms();

        if (w->opened < w->nconns) {
            int allowed = w->nconns;
            if (rate_per_ms > 0) {
                double a = (double)(now - start + 1) * rate_per_ms;
                if (a < allowed)
                    allowed = (int)a;
            }
            while (w->opened < allowed)
                conn_open(w, &w->conns[w->opened++]);
        }

        timer_run(w, now);

        int timeout = (w->opened < w->nconns || w->wheel_count > 0) ? 1 : 50;
        int n = epoll_wait(w->epfd, evs, MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            err(EXIT_FAILURE, "epoll_wait");
        }

        for (int i = 0; i < n; i++) {
            struct conn *c = evs[i].data.ptr;
            uint32_t e = evs[i].events;

            if (c->state == ST_CLOSED)
                continue;
            if (c->state == ST_CONNECTING) {
                on_connected(w, c);
                continue;
            }
            if (e & EPOLLIN)
                on_readable(w, c);
            if (c->state == ST_CLOSED)
                continue;
            if (e & (EPOLLERR | EPOLLHUP)) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
                conn_close(w, c, soerr ? soerr : ECONNRESET);
                continue;
            }
            if ((e & EPOLLOUT) && c->state == ST_INFLIGHT)
                do_send(w, c);
        }
    }

    for (int i = 0; i < w->opened; i++)
        if (w->conns[i].state != ST_CLOSED && w->conns[i].fd >= 0)
            close(w->conns[i].fd);
    return NULL;
}

/* ---------- setup ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options] <address> <port>\n"
            "  -c N   concurrent connections        (default 10000)\n"
            "  -t N   worker threads                (default 2)\n"
            "  -d S   test duration in seconds      (default 30)\n"
            "  -s B   message size in bytes         (default 64)\n"
            "  -i MS  think time between messages   (default 0 = flat out)\n"
            "  -r N   new connections per second    (default 5000, 0 = no limit)\n",
            prog);
    exit(EXIT_FAILURE);
}

static long parse_long(const char *s, long min, long max, const char *what) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        errx(EXIT_FAILURE, "invalid %s: %s", what, s);
    return v;
}

static void resolve(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0)
        errx(EXIT_FAILURE, "address %s port %s: %s", host, port, gai_strerror(rc));
    memcpy(&cfg.addr, res->ai_addr, res->ai_addrlen);
    cfg.addrlen = res->ai_addrlen;
    freeaddrinfo(res);
}

static void raise_fd_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
        if (rl.rlim_cur < (rlim_t)cfg.conns + 64)
            warnx("fd limit is %llu but %d connections need more; "
                  "raise it with `ulimit -n`",
                  (unsigned long long)rl.rlim_cur, cfg.conns);
    }
}

static void handle_sigint(int sig) {
    (void)sig;
    got_sigint = 1;
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "c:t:d:s:i:r:h")) != -1) {
        switch (opt) {
        case 'c': cfg.conns = (int)parse_long(optarg, 1, 10000000, "connections"); break;
        case 't': cfg.threads = (int)parse_long(optarg, 1, 1024, "threads"); break;
        case 'd': cfg.duration_s = (int)parse_long(optarg, 1, 86400, "duration"); break;
        case 's': cfg.msg_size = (uint32_t)parse_long(optarg, 1, 1 << 20, "message size"); break;
        case 'i': cfg.interval_ms = (uint32_t)parse_long(optarg, 0, 3600000, "interval"); break;
        case 'r': cfg.connect_rate = (int)parse_long(optarg, 0, 10000000, "connect rate"); break;
        default: usage(argv[0]);
        }
    }
    if (argc - optind != 2)
        usage(argv[0]);
    resolve(argv[optind], argv[optind + 1]);
    if (cfg.threads > cfg.conns)
        cfg.threads = cfg.conns;
    raise_fd_limit();

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Workers inherit a mask with SIGINT blocked, so only main receives it. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    pthread_sigmask(SIG_BLOCK, &block, &old);

    struct worker **ws = calloc((size_t)cfg.threads, sizeof(*ws));
    if (!ws)
        err(EXIT_FAILURE, "calloc");
    for (int t = 0; t < cfg.threads; t++) {
        struct worker *w = calloc(1, sizeof(*w));
        if (!w)
            err(EXIT_FAILURE, "calloc worker");
        w->nconns = cfg.conns / cfg.threads + (t < cfg.conns % cfg.threads);
        w->conns = calloc((size_t)w->nconns, sizeof(struct conn));
        w->arena = malloc((size_t)w->nconns * cfg.msg_size);
        if (!w->conns || !w->arena)
            err(EXIT_FAILURE, "allocating connections");
        w->rng = 0x9e3779b9u ^ (uint32_t)(t + 1) * 2654435761u;
        for (int i = 0; i < w->nconns; i++) {
            struct conn *c = &w->conns[i];
            c->fd = -1;
            c->state = ST_IDLE;
            c->msg = w->arena + (size_t)i * cfg.msg_size;
            for (uint32_t b = 0; b < cfg.msg_size; b++)
                c->msg[b] = (unsigned char)xorshift32(&w->rng);
        }
        w->epfd = epoll_create1(EPOLL_CLOEXEC);
        if (w->epfd < 0)
            err(EXIT_FAILURE, "epoll_create1");
        ws[t] = w;
    }

    printf("target %d conns, %d threads, %u-byte msgs, think %u ms, ramp %d/s, %d s\n",
           cfg.conns, cfg.threads, cfg.msg_size, cfg.interval_ms, cfg.connect_rate,
           cfg.duration_s);
    printf("%5s %8s %8s %8s %8s %10s %9s\n", "time", "active", "conn_ok", "conn_err",
           "errors", "msgs/s", "MiB/s");

    uint64_t t0 = now_ms();
    for (int t = 0; t < cfg.threads; t++) {
        int rc = pthread_create(&ws[t]->tid, NULL, worker_main, ws[t]);
        if (rc != 0)
            errx(EXIT_FAILURE, "pthread_create: %s", strerror(rc));
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    uint64_t prev_msgs = 0, prev_bytes = 0, prev_t = t0;
    while (!got_sigint) {
        struct timespec ts = {.tv_sec = 1};
        nanosleep(&ts, NULL);

        uint64_t now = now_ms();
        uint64_t active = 0, ok = 0, cfail = 0, errs = 0, msgs = 0, bytes = 0;
        for (int t = 0; t < cfg.threads; t++) {
            active += stat_get(&ws[t]->st.active);
            ok += stat_get(&ws[t]->st.connected);
            cfail += stat_get(&ws[t]->st.connect_fail);
            errs += stat_get(&ws[t]->st.errors);
            msgs += stat_get(&ws[t]->st.msgs);
            bytes += stat_get(&ws[t]->st.bytes);
        }
        double dt = (double)(now - prev_t) / 1000.0;
        if (dt <= 0)
            dt = 1;
        printf("%4.0fs %8llu %8llu %8llu %8llu %10.0f %9.2f\n",
               (double)(now - t0) / 1000.0, (unsigned long long)active,
               (unsigned long long)ok, (unsigned long long)cfail,
               (unsigned long long)errs, (double)(msgs - prev_msgs) / dt,
               (double)(bytes - prev_bytes) / dt / (1024.0 * 1024.0));
        fflush(stdout);
        prev_msgs = msgs;
        prev_bytes = bytes;
        prev_t = now;
        if (now - t0 >= (uint64_t)cfg.duration_s * 1000)
            break;
    }

    atomic_store(&stop_flag, true);
    for (int t = 0; t < cfg.threads; t++)
        pthread_join(ws[t]->tid, NULL);
    double elapsed = (double)(now_ms() - t0) / 1000.0;

    static uint64_t hist[HIST_BUCKETS];
    uint64_t ok = 0, cfail = 0, errs = 0, mism = 0, msgs = 0, bytes = 0, max_us = 0;
    int last_cerr = 0, last_err = 0;
    for (int t = 0; t < cfg.threads; t++) {
        struct worker *w = ws[t];
        ok += stat_get(&w->st.connected);
        cfail += stat_get(&w->st.connect_fail);
        errs += stat_get(&w->st.errors);
        mism += stat_get(&w->st.mismatches);
        msgs += stat_get(&w->st.msgs);
        bytes += stat_get(&w->st.bytes);
        if (w->lat_max_us > max_us)
            max_us = w->lat_max_us;
        if (w->st.last_connect_errno)
            last_cerr = w->st.last_connect_errno;
        if (w->st.last_errno)
            last_err = w->st.last_errno;
        for (int i = 0; i < HIST_BUCKETS; i++)
            hist[i] += w->hist[i];
    }

    printf("\n=== summary (%.1f s) ===\n", elapsed);
    printf("connections: %llu ok, %llu failed%s%s\n", (unsigned long long)ok,
           (unsigned long long)cfail, last_cerr ? ", last error: " : "",
           last_cerr ? strerror(last_cerr) : "");
    printf("errors:      %llu%s%s, echo mismatches: %llu\n", (unsigned long long)errs,
           last_err ? ", last: " : "", last_err ? strerror(last_err) : "",
           (unsigned long long)mism);
    printf("messages:    %llu (%.0f msg/s, %.2f MiB/s echoed)\n",
           (unsigned long long)msgs, (double)msgs / elapsed,
           (double)bytes / elapsed / (1024.0 * 1024.0));
    printf("latency us:  p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu\n",
           (unsigned long long)hist_percentile(hist, msgs, 0.50),
           (unsigned long long)hist_percentile(hist, msgs, 0.90),
           (unsigned long long)hist_percentile(hist, msgs, 0.99),
           (unsigned long long)hist_percentile(hist, msgs, 0.999),
           (unsigned long long)max_us);

    return (mism > 0 || cfail > 0) ? 1 : 0;
}