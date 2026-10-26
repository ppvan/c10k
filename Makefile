CC     = /usr/bin/clang
CFLAGS = -std=c17 -g3 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
         -Wformat=2 -Wnull-dereference -Wimplicit-fallthrough -pthread -D_GNU_SOURCE
SAN    = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

.PHONY: all debug tsan release clean test

all: debug

test: debug
	./build/loop_test.out

debug:
	$(CC) $(CFLAGS) -O0 $(SAN) -o build/loop_test.out src/loop_test.c src/loop.c
	$(CC) $(CFLAGS) -O0 $(SAN) -o build/main.out src/main.c src/loop.c

tsan:
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o build/loop_test.out src/loop_test.c src/loop.c
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -o build/main.out src/main.c src/loop.c

release:
	$(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o bin/client.out src/client.c
	$(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o bin/server_poll_release.out src/server_poll.c
	$(CC) $(CFLAGS) -O2 -D_FORTIFY_SOURCE=2 -o bin/server_thread_release.out src/server_thread.c


clean:
	rm -f build/*.out