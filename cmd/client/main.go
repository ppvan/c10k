// c10k load client: opens many concurrent connections to an echo server,
// keeps them open, and measures round-trip latency per message.
//
//	go run . -conns 10000 -rate 1000 -interval 1s -duration 30s
package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"os/signal"
	"sort"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

var (
	addr     = flag.String("addr", "127.0.0.1:8080", "server address")
	conns    = flag.Int("conns", 10000, "number of concurrent connections")
	rate     = flag.Int("rate", 1000, "new connections per second during ramp-up")
	interval = flag.Duration("interval", time.Second, "time between messages on each connection")
	duration = flag.Duration("duration", 30*time.Second, "how long to hold connections after ramp-up")
	msgSize  = flag.Int("size", 32, "message size in bytes")
	timeout  = flag.Duration("timeout", 5*time.Second, "dial and per-message timeout")
)

type stats struct {
	active atomic.Int64
	msgs   atomic.Int64

	mu     sync.Mutex
	rtts   []time.Duration
	errors map[string]int
}

func (s *stats) addRTT(d time.Duration) {
	s.mu.Lock()
	s.rtts = append(s.rtts, d)
	s.mu.Unlock()
}

func (s *stats) addErr(phase string, err error) {
	s.mu.Lock()
	s.errors[phase+": "+classify(err)]++
	s.mu.Unlock()
}

// classify groups errors by cause so the summary stays readable.
func classify(err error) string {
	var ne net.Error
	switch {
	case errors.As(err, &ne) && ne.Timeout():
		return "timeout"
	case errors.Is(err, syscall.ECONNREFUSED):
		return "connection refused"
	case errors.Is(err, syscall.ECONNRESET):
		return "connection reset"
	case errors.Is(err, syscall.EMFILE):
		return "too many open files (raise client ulimit -n)"
	case errors.Is(err, syscall.EADDRNOTAVAIL):
		return "out of ephemeral ports"
	case errors.Is(err, io.EOF), errors.Is(err, io.ErrUnexpectedEOF):
		return "server closed connection"
	default:
		return err.Error()
	}
}

func worker(s *stats, stop <-chan struct{}, wg *sync.WaitGroup) {
	defer wg.Done()

	c, err := net.DialTimeout("tcp", *addr, *timeout)
	if err != nil {
		s.addErr("dial", err)
		return
	}
	defer c.Close()
	s.active.Add(1)
	defer s.active.Add(-1)

	msg := make([]byte, *msgSize)
	for i := range msg {
		msg[i] = 'a' + byte(i%26)
	}
	msg[len(msg)-1] = '\n'
	buf := make([]byte, len(msg))

	// Random start offset so 10k connections don't all send in the same instant.
	select {
	case <-stop:
		return
	case <-time.After(time.Duration(rand.Int63n(int64(*interval)))):
	}

	ticker := time.NewTicker(*interval)
	defer ticker.Stop()
	for {
		start := time.Now()
		c.SetDeadline(start.Add(*timeout))
		if _, err := c.Write(msg); err != nil {
			s.addErr("write", err)
			return
		}
		// ReadFull: TCP is a byte stream, the echo may arrive in pieces.
		if _, err := io.ReadFull(c, buf); err != nil {
			s.addErr("read", err)
			return
		}
		s.addRTT(time.Since(start))
		s.msgs.Add(1)

		select {
		case <-stop:
			return
		case <-ticker.C:
		}
	}
}

func main() {
	flag.Parse()
	if *msgSize < 1 || *rate < 1 || *interval <= 0 {
		fmt.Fprintln(os.Stderr, "size, rate and interval must be positive")
		os.Exit(2)
	}

	s := &stats{errors: make(map[string]int)}
	stop := make(chan struct{})
	var wg sync.WaitGroup

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)

	// Progress line every second.
	go func() {
		var last int64
		for range time.Tick(time.Second) {
			m := s.msgs.Load()
			s.mu.Lock()
			nerr := 0
			for _, n := range s.errors {
				nerr += n
			}
			s.mu.Unlock()
			fmt.Printf("active=%-6d msgs/s=%-7d errors=%d\n", s.active.Load(), m-last, nerr)
			last = m
		}
	}()

	fmt.Printf("ramping up %d connections to %s at %d/s\n", *conns, *addr, *rate)
	tick := time.NewTicker(time.Second / time.Duration(*rate))
ramp:
	for i := 0; i < *conns; i++ {
		select {
		case <-sig:
			break ramp
		case <-tick.C:
		}
		wg.Add(1)
		go worker(s, stop, &wg)
	}
	tick.Stop()

	fmt.Printf("ramp-up done, holding for %s (Ctrl+C to stop early)\n", *duration)
	select {
	case <-sig:
	case <-time.After(*duration):
	}
	close(stop)
	wg.Wait()

	fmt.Println("\n=== summary ===")
	fmt.Printf("messages: %d\n", s.msgs.Load())
	if n := len(s.rtts); n > 0 {
		sort.Slice(s.rtts, func(i, j int) bool { return s.rtts[i] < s.rtts[j] })
		p := func(q float64) time.Duration { return s.rtts[int(q*float64(n-1))] }
		fmt.Printf("rtt p50=%v p90=%v p99=%v max=%v\n", p(0.50), p(0.90), p(0.99), s.rtts[n-1])
	}
	if len(s.errors) > 0 {
		fmt.Println("errors:")
		for k, v := range s.errors {
			fmt.Printf("  %6d  %s\n", v, k)
		}
	}
}
