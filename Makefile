CC     = /usr/bin/clang
CFLAGS = -std=c17 -g3 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
         -Wformat=2 -Wnull-dereference -Wimplicit-fallthrough -pthread
SAN    = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

.PHONY: all debug tsan release clean

all: debug release

debug:
	$(CC) $(CFLAGS) -O0 $(SAN) -o bin/server.out src/server.c

tsan:
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o bin/server_tsan.out src/server.c

release:
	$(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o bin/server_rel.out src/server.c


clean:
	rm -f bin/*.out