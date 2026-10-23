Milestone 1: No single connection can kill the server

Right now, several per-connection failures end the whole process:

SIGPIPE. This is the biggest hidden crash. If a client disconnects while a thread is in send, the kernel sends SIGPIPE, and its default action terminates the process silently, with no error message. Under load, with clients timing out and resetting connections, this will happen eventually. The fix is either MSG_NOSIGNAL on every send or ignoring SIGPIPE process-wide. Either way, send then fails with EPIPE, which you already handle.

accept errors. Most of them are about one connection or a temporary condition, not the server: ECONNABORTED (client gave up while queued), EMFILE/ENFILE (out of fds), ENOBUFS/ENOMEM. Log them and keep looping. EMFILE is tricky: the pending connection stays in the queue, so accept fails again immediately and you spin at 100% CPU. Look up the “reserve fd” trick (keep a spare fd open, release it to accept-and-close the excess connection, then reopen it), or at least back off briefly.
pthread_create failure. Close that one peer_fd, log, count it, and keep serving existing connections. This turns the level-2 crash at around 32k threads into load shedding, which is what a robust server does.
malloc failure in the worker should close the connection, not crash.

Done when: the server stays up through ulimit -n 1000 with 2000 clients, a thread limit you hit on purpose, and clients killed with kill -9 in the middle of a request.

Milestone 2: Logging you can actually use under load
Include context in every line: a timestamp, a level (error/warn/info), the thread id, and the peer address. Switch peer_addr to struct sockaddr_storage (so it works for IPv6 too) and format it with inet_ntop.
Write each log line as a single call. Build the full line in a buffer and write it at once, so lines from different threads don’t interleave.
Rate-limit or aggregate. When the server is in trouble, it can produce 10k identical errors per second, and logging them all becomes the next bottleneck. Log the first occurrence, then a periodic summary like “EMFILE ×4213 in last 1s”.
Validate startup input (strtol with a range check instead of atoi) so a bad port gives a clear message.

Done when: during a failure test you can tell from the log alone what failed, how often, and for which clients.

Milestone 3: Bound every resource

A robust server decides its own limits instead of waiting for the OS to enforce them:

Cap the number of connections with an atomic counter. Above the cap, close new connections immediately (for HTTP, send a 503 first). This is more predictable than hitting pthread_create failures.
Add timeouts to each connection socket with SO_RCVTIMEO and SO_SNDTIMEO, so idle clients, slowloris clients, and slow readers can’t hold a thread forever. This is what lets you pass level 3 with threads at all.
Use a smaller thread stack via pthread_attr_setstacksize (e.g. 64–256 KB). It won’t raise the max_map_count thread limit, but it cuts VSZ dramatically and makes your memory numbers clearer.

Done when: the connection count plateaus at your cap under a flood, and slowloris connections get dropped after the timeout.

Milestone 4: Graceful shutdown

At the moment, Ctrl+C returns from main and kills every detached thread mid-request. A graceful shutdown instead:

stops accepting and closes the listening socket,
waits for active connections to finish, tracked with a counter plus a condition variable,
enforces a deadline, then exits.

Also add SIGTERM to the handler, since that’s what process managers send.

Done when: Ctrl+C during a load test finishes the in-flight requests (no resets on the client side), and the server exits within the deadline.

Milestone 5: Server-side metrics

You’ve been measuring everything from the client. Keep atomic counters in the server for active connections, total accepted, rejected for being over the cap, timeouts, and errors by type. Print them once per second, or on SIGUSR1. This is what makes “the server erroring out under load” visible from the server’s side, and it gives the epoll version a direct comparison.

Milestone 6: Fault-injection checklist

Write down each failure you’ve handled and a command that triggers it: a low fd limit, a low thread limit, killed clients, slowloris, slow readers, Ctrl+C under load. Run the whole list against the release build and again under ASan and TSan. Keep the list, because the epoll version has to pass the same tests, and that comparison is the most valuable result of the project.