CC     = /usr/bin/clang
CFLAGS = -std=c23 -g3 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
         -Wformat=2 -Wnull-dereference -Wimplicit-fallthrough -pthread -D_GNU_SOURCE
SAN    = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

.PHONY: all debug tsan release clean test

all: debug

test: debug
	./build/loop_test.out

debug:
	$(CC) $(CFLAGS) -O0 $(SAN) -o build/loop_test.out src/loop_test.c src/loop.c
	$(CC) $(CFLAGS) -O0 $(SAN) -o build/main.out src/main.c src/loop.c src/conn.c
	$(CC) $(CFLAGS) -O0 $(SAN) -o build/client.out src/client.c src/loop.c src/cli.c src/conn.c
	$(CC) $(CFLAGS) -O0 $(SAN) -o build/echo.out src/echo.c src/loop.c src/cli.c src/conn.c

tsan:
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o build/loop_test.out src/loop_test.c src/loop.c
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o build/main.out src/main.c src/loop.c

release:
	$(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o build/client.out src/client.c src/loop.c src/cli.c src/conn.c
	$(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o build/echo_release.out src/echo.c src/loop.c src/cli.c src/conn.c
	$(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o build/echo_poll.out src/server_poll.c


clean:
	rm -f build/*.out