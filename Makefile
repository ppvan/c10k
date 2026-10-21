CC      = /usr/bin/clang
CFLAGS  = -std=c17 -g3 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
          -Wformat=2 -Wnull-dereference -Wimplicit-fallthrough
SAN     = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

debug:   ; $(CC) $(CFLAGS) -O0 $(SAN) -o server server.c
tsan:    ; $(CC) $(CFLAGS) -O1 -fsanitize=thread -o server_tsan server.c
release: ; $(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o server_rel server.c