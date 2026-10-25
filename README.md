# C10k

This project is my playground for learning about the [C10k problem](http://www.kegel.com/c10k.html). I hand-code a TCP echo server in C and load-test it with 10k+ concurrent connections using a custom epoll-based client.

## Environment

- Device: WSL2
- CPU: AMD Ryzen 7 7735H
- RAM: 32 GB
- GPU: AMD Radeon (4 GB)



## Results

### Thread per connection

On modern hardware, C10k is no longer a hardware problem. Even a naive **thread-per-connection** server scales well past 10k connections. What stops it first are conservative **OS default limits**, not CPU or RAM. Each limit below showed up as a hard failure as connection count grew.

| Limit | Default (this machine) | Symptom | Fix |
|---|---|---|---|
| **systemd scope `TasksMax`** (cgroup `pids.max`, 15% of the kernel task limit) | 16,570 | `pthread_create: Resource temporarily unavailable` (EAGAIN); new connections accepted then closed immediately | `systemd-run --user --scope -p TasksMax=infinity ./server …` |
| **`RLIMIT_NOFILE`** (soft `ulimit -n`) | 10,240 | `accept: Too many open files` (EMFILE); active connection count freezes | raise soft limit to hard in code (`setrlimit`), or `prlimit --nofile=1048576:1048576` |
| **systemd user slice `TasksMax`** (33% of the kernel task limit; `user@UID.service` is also capped) | ~36,455 | `pthread_create` EAGAIN | `systemctl set-property --runtime user-UID.slice TasksMax=infinity` (same for `user@UID.service`) |
| **`RLIMIT_NPROC`** (`ulimit -u`, counts *all* tasks of the user) | 55,235 | `pthread_create` EAGAIN | `sudo prlimit --pid <shell_pid> --nproc=unlimited:unlimited` (children inherit) |
| **`kernel.threads-max`** (computed from RAM) | 110,471 | `pthread_create` EAGAIN | `sysctl -w kernel.threads-max=4000000` |


With all limits bypassed, the machine handled **~200k connections** with plain thread-per-connection.

**Cost per connection**

At 32k connections, the server used ~3.8 GB RSS. That's **~118 KB user memory per connection**, plus a 16 KB unswappable kernel stack, a PID and a scheduler entry per thread. All that holds an echo connection whose real state is one fd and a 1 KB buffer. This overhead, and the limits that exist to contain it, are what event-driven designs remove.


### Select

Select has a hard limit of 1024 because of `fd_set` type. Making it not suitable for c10k. This can't be bypassed without recompile linux..
Try to set a high fd to `fd_set` is underfined, and trigger asan on my machine.

```c
int high_fd = fcntl(fd, F_DUPFD, 1025);
printf("fd = %d, FD_SETSIZE = %d\n", high_fd, FD_SETSIZE);

fd_set readfds;
FD_ZERO(&readfds);
FD_SET(high_fd, &readfds);  // Undefined behavior!

==490180==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x7afa2b2f00a0 at pc 0x62214f0c90ab bp 0x7ffdff42f9d0 sp 0x7ffdff42f9c8
READ of size 8 at 0x7afa2b2f00a0 thread T0
    #0 0x62214f0c90aa in main /home/ppvan/Documents/c10k/src/demo_select.c:16:5
    #1 0x7efa2d427780  (/usr/lib/libc.so.6+0x27780) (BuildId: d320029ad1a23be2f4bce117fd321cbd23a2014b)
    #2 0x7efa2d4278b8 in __libc_start_main (/usr/lib/libc.so.6+0x278b8) (BuildId: d320029ad1a23be2f4bce117fd321cbd23a2014b)
    #3 0x62214efd1124 in _start (/home/ppvan/Documents/c10k/bin/demo_select.out+0x2e124) (BuildId: ffb459fa87f44d6ef8e025a4a89e535d64e44eb0)

Address 0x7afa2b2f00a0 is located in stack of thread T0 at offset 160 in frame
    #0 0x62214f0c8cb7 in main /home/ppvan/Documents/c10k/src/demo_select.c:7
```

### Epoll

