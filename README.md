# C10k

This project is my playground for learning about the [C10k problem](http://www.kegel.com/c10k.html). I hand-code a TCP echo server in C and load-test it with 10k+ concurrent connections using a custom epoll-based client.

## Environment

| | |
|---|---|
| **Device** | WSL2 |
| **CPU** | AMD Ryzen 7 7735H |
| **RAM** | 32 GB |
| **GPU** | AMD Radeon (4 GB) |

## Results

- [Thread per connection](#thread-per-connection)
- [Select](#select)
- [Poll](#poll)
- [Epoll](#epoll)

---

### Thread per connection

On modern hardware, C10k is no longer a hardware problem. Even a naive **thread-per-connection** server scales well past 10k connections. What stops it first are conservative **OS default limits**, not CPU or RAM. Each limit below showed up as a hard failure as connection count grew.

| Limit | Default (this machine) | Symptom | Fix |
|---|---|---|---|
| **systemd scope `TasksMax`** (cgroup `pids.max`, 15% of the kernel task limit) | 16,570 | `pthread_create: Resource temporarily unavailable` (EAGAIN); new connections accepted then closed immediately | `systemd-run --user --scope -p TasksMax=infinity ./server …` |
| **`RLIMIT_NOFILE`** (soft `ulimit -n`) | 10,240 | `accept: Too many open files` (EMFILE); active connection count freezes | Raise soft limit to hard in code (`setrlimit`), or `prlimit --nofile=1048576:1048576` |
| **systemd user slice `TasksMax`** (33% of the kernel task limit; `user@UID.service` is also capped) | ~36,455 | `pthread_create` EAGAIN | `systemctl set-property --runtime user-UID.slice TasksMax=infinity` (same for `user@UID.service`) |
| **`RLIMIT_NPROC`** (`ulimit -u`, counts *all* tasks of the user) | 55,235 | `pthread_create` EAGAIN | `sudo prlimit --pid <shell_pid> --nproc=unlimited:unlimited` (children inherit) |
| **`kernel.threads-max`** (computed from RAM) | 110,471 | `pthread_create` EAGAIN | `sysctl -w kernel.threads-max=4000000` |

With all limits bypassed, the machine handled **~200k connections** with plain thread-per-connection before WSL2 shut itself down.

#### Cost per connection

At 32k connections, the server used ~3.8 GB RSS. That's **~118 KB user memory per connection**, plus a 16 KB unswappable kernel stack, a PID and a scheduler entry per thread. All that holds an echo connection whose real state is one fd and a 1 KB buffer. This overhead, and the limits that exist to contain it, are what event-driven designs remove.

---

### Select

Select has a hard limit of **1024** because of the `fd_set` type, making it not suitable for C10k. This can't be bypassed without recompiling Linux.

Trying to set a high fd in an `fd_set` is undefined behavior, and it triggers ASan on my machine:

```c
int high_fd = fcntl(fd, F_DUPFD, 1025);
printf("fd = %d, FD_SETSIZE = %d\n", high_fd, FD_SETSIZE);

fd_set readfds;
FD_ZERO(&readfds);
FD_SET(high_fd, &readfds);  // Undefined behavior!
```

```text
==490180==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x7afa2b2f00a0 at pc 0x62214f0c90ab bp 0x7ffdff42f9d0 sp 0x7ffdff42f9c8
READ of size 8 at 0x7afa2b2f00a0 thread T0
    #0 0x62214f0c90aa in main /home/ppvan/Documents/c10k/src/demo_select.c:16:5
    #1 0x7efa2d427780  (/usr/lib/libc.so.6+0x27780) (BuildId: d320029ad1a23be2f4bce117fd321cbd23a2014b)
    #2 0x7efa2d4278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: d320029ad1a23be2f4bce117fd321cbd23a2014b)
    #3 0x62214efd1124 in _start (/home/ppvan/Documents/c10k/bin/demo_select.out+0x2e124) (BuildId: ffb459fa87f44d6ef8e025a4a89e535d64e44eb0)

Address 0x7afa2b2f00a0 is located in stack of thread T0 at offset 160 in frame
    #0 0x62214f0c8cb7 in main /home/ppvan/Documents/c10k/src/demo_select.c:7
```

---

### Poll

`poll` low-key handles **100k connections on 1 CPU core with ~100 MB RSS**, so much better than threads. However, it scales linearly with the number of fds, as shown below. I try to convince myself that `30ms` per event loop iteration is not acceptable for a low-level socket server. Is it?

It will only get better from here with `epoll`, I think?

| fds | iters/s | ready/iter | poll (ms/iter) | dispatch (ms/iter) |
|---:|---:|---:|---:|---:|
| 4,791 | 264 | 460 | 0.45 | 3.3 |
| 9,715 | 18 | 6,917 | 2.1 | 52.0 |
| 24,845 | 6 | 22,218 | 7.1 | 165.9 |
| 53,828 | 3 | 48,525 | 15.9 | 382.3 |
| 73,016 | 2 | 70,895 | 22.2 | 557.7 |
| 100,001 | 1 | 99,999 | 32.0 | 778.9 |

<details>
<summary>Full log</summary>

```text
fds    4791 | iters/s     264 | ready/iter    460.4 | poll   0.446 ms/iter | dispatch   3.341 ms/iter | shed 0
fds    9715 | iters/s      18 | ready/iter   6916.5 | poll   2.113 ms/iter | dispatch  51.968 ms/iter | shed 0
fds   15265 | iters/s      11 | ready/iter  12045.5 | poll   3.680 ms/iter | dispatch  89.789 ms/iter | shed 0
fds   19801 | iters/s       7 | ready/iter  17009.9 | poll   5.560 ms/iter | dispatch 128.283 ms/iter | shed 0
fds   24845 | iters/s       6 | ready/iter  22217.8 | poll   7.065 ms/iter | dispatch 165.908 ms/iter | shed 0
fds   31361 | iters/s       5 | ready/iter  27382.6 | poll   8.545 ms/iter | dispatch 213.497 ms/iter | shed 0
fds   36488 | iters/s       4 | ready/iter  32595.5 | poll  10.171 ms/iter | dispatch 246.358 ms/iter | shed 0
fds   42516 | iters/s       3 | ready/iter  37940.8 | poll  11.943 ms/iter | dispatch 289.435 ms/iter | shed 0
fds   46061 | iters/s       3 | ready/iter  44282.0 | poll  15.103 ms/iter | dispatch 340.859 ms/iter | shed 0
fds   53828 | iters/s       3 | ready/iter  48525.3 | poll  15.858 ms/iter | dispatch 382.293 ms/iter | shed 0
fds   60212 | iters/s       2 | ready/iter  55285.7 | poll  17.321 ms/iter | dispatch 437.404 ms/iter | shed 0
fds   64579 | iters/s       2 | ready/iter  61667.0 | poll  19.614 ms/iter | dispatch 470.143 ms/iter | shed 0
fds   68774 | iters/s       2 | ready/iter  66676.0 | poll  21.188 ms/iter | dispatch 517.916 ms/iter | shed 0
fds   73016 | iters/s       2 | ready/iter  70894.5 | poll  22.226 ms/iter | dispatch 557.650 ms/iter | shed 0
fds   82083 | iters/s       2 | ready/iter  75111.5 | poll  23.452 ms/iter | dispatch 616.077 ms/iter | shed 0
fds   86576 | iters/s       1 | ready/iter  82081.5 | poll  26.654 ms/iter | dispatch 643.204 ms/iter | shed 0
fds   91111 | iters/s       1 | ready/iter  86574.5 | poll  27.169 ms/iter | dispatch 731.199 ms/iter | shed 0
fds   95423 | iters/s       1 | ready/iter  93265.5 | poll  33.081 ms/iter | dispatch 762.185 ms/iter | shed 0
fds   99520 | iters/s       1 | ready/iter  97470.0 | poll  30.654 ms/iter | dispatch 766.307 ms/iter | shed 0
fds   99968 | iters/s       1 | ready/iter  99742.5 | poll  30.715 ms/iter | dispatch 777.429 ms/iter | shed 0
fds  100001 | iters/s       1 | ready/iter  99966.5 | poll  32.198 ms/iter | dispatch 855.728 ms/iter | shed 0
fds  100001 | iters/s       1 | ready/iter  99999.0 | poll  31.975 ms/iter | dispatch 778.910 ms/iter | shed 0
fds  100001 | iters/s       1 | ready/iter  99999.0 | poll  31.304 ms/iter | dispatch 785.302 ms/iter | shed 0
```

</details>

---

### Epoll

_TODO_