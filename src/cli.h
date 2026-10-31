// Command-line handling shared by network servers (echo, http, ...).
//
// Common usage:  prog [-v] [-n backlog] [extra opts] <address> <port>

#ifndef CLI_H
#define CLI_H

#include <stdbool.h>
#include <stdint.h>

struct cli_config {
    const char *address;
    uint16_t port;
    int backlog;
    bool verbose;
};

// Optional hook so a server can add its own options without reimplementing
// the parser. Pass NULL to cli_parse() if you don't need it.
struct cli_ext {
    const char *optstring; // extra getopt letters, e.g. "r:t"
    const char *help;      // extra usage lines, e.g. "  -r <dir>   document root\n"
    bool (*handle)(int opt, const char *arg, void *ud);
    void *ud;
};

void cli_parse(struct cli_config *cfg, int argc, char **argv,
               const struct cli_ext *ext);

long cli_parse_long(const char *raw, const char *what, long min, long max);

#endif