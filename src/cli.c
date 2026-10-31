#include "cli.h"

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

__attribute__((noreturn)) static void usage(const char *prog, int status,
                                            const struct cli_ext *ext) {
    fprintf(status == EXIT_SUCCESS ? stdout : stderr,
            "usage: %s [-v] [-n backlog] <address> <port>\n"
            "  <address>    IPv4 or IPv6 address to bind (e.g. 127.0.0.1, ::1)\n"
            "  <port>       port number (1-65535)\n"
            "  -v           verbose output\n"
            "  -n <int>     listen() backlog (default SOMAXCONN = %d)\n"
            "  -h           show this help\n"
            "%s",
            prog, SOMAXCONN, ext && ext->help ? ext->help : "");
    exit(status);
}

long cli_parse_long(const char *raw, const char *what, long min, long max) {
    char *end = NULL;

    errno = 0;
    long value = strtol(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || value < min || value > max)
        errx(EXIT_FAILURE, "invalid %s: '%s' (expected %ld-%ld)", what, raw, min,
             max);
    return value;
}

void cli_parse(struct cli_config *cfg, int argc, char **argv,
               const struct cli_ext *ext) {
    char optstring[64];
    int opt;

    memset(cfg, 0, sizeof *cfg);
    cfg->backlog = SOMAXCONN;

    snprintf(optstring, sizeof optstring, "hvn:%s",
             ext && ext->optstring ? ext->optstring : "");

    while ((opt = getopt(argc, argv, optstring)) != -1) {
        switch (opt) {
        case 'v':
            cfg->verbose = true;
            break;
        case 'n':
            cfg->backlog = (int)cli_parse_long(optarg, "backlog", 1, INT_MAX);
            break;
        case 'h':
            usage(argv[0], EXIT_SUCCESS, ext);
        case '?':
            usage(argv[0], EXIT_FAILURE, ext);
        default:
            if (ext == NULL || ext->handle == NULL ||
                !ext->handle(opt, optarg, ext->ud))
                usage(argv[0], EXIT_FAILURE, ext);
        }
    }
    if (argc - optind != 2)
        usage(argv[0], EXIT_FAILURE, ext);

    cfg->address = argv[optind];
    cfg->port = (uint16_t)cli_parse_long(argv[optind + 1], "port", 1, 65535);
}