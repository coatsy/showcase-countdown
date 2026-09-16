package main

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"
)

// net.Pipe supplies a real byte stream without opening a hardware serial port.
type bridgeTestPort struct{ net.Conn }

func (p bridgeTestPort) SetReadTimeout(time.Duration) error { return nil }
func (p bridgeTestPort) Read(buf []byte) (int, error) {
	_ = p.SetReadDeadline(time.Now().Add(10 * time.Millisecond))
	n, err := p.Conn.Read(buf)
	if e, ok := err.(net.Error); ok && e.Timeout() {
		return n, nil
	}
	return n, err
}

type bridgeTestPeer struct {
	conn  net.Conn
	lines chan string
}

func newBridgeTestPipe(t *testing.T) (bridgePort, *bridgeTestPeer) {
	t.Helper()
	host, device := net.Pipe()
	peer := &bridgeTestPeer{conn: device, lines: make(chan string, 256)}
	done := make(chan struct{})
	stop := make(chan struct{})
	go func() {
		defer close(done)
		scanner := bufio.NewScanner(device)
		for scanner.Scan() {
			select {
			case peer.lines <- scanner.Text():
			case <-stop:
				return
			}
		}
	}()
	t.Cleanup(func() {
		close(stop)
		_ = device.Close()
		_ = host.Close()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("serial peer reader did not stop")
		}
	})
	return bridgeTestPort{host}, peer
}

func (p *bridgeTestPeer) write(t *testing.T, text string) {
	t.Helper()
	_ = p.conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	if _, err := p.conn.Write([]byte(text)); err != nil {
		t.Fatalf("device serial write: %v", err)
	}
}

func (p *bridgeTestPeer) next(t *testing.T) string {
	t.Helper()
	select {
	case line := <-p.lines:
		return line
	case <-time.After(3 * time.Second):
		t.Fatal("timed out waiting for serial output")
		return ""
	}
}

func (p *bridgeTestPeer) quiet(t *testing.T) {
	t.Helper()
	select {
	case line := <-p.lines:
		t.Fatalf("unexpected serial output: %s", line)
	case <-time.After(80 * time.Millisecond):
	}
}

func bridgeEventually(t *testing.T, what string, check func() bool) {
	t.Helper()
	deadline := time.Now().Add(4 * time.Second)
	for time.Now().Before(deadline) {
		if check() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

func bridgeIsConnected(b *SerialBridge) bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.connected
}

func runBridgeTest(t *testing.T, b *SerialBridge) (*bridgeTestPeer, chan bridgePort) {
	t.Helper()
	port, peer := newBridgeTestPipe(t)
	ports := make(chan bridgePort, 2)
	ports <- port
	b.open = func() (bridgePort, error) {
		select {
		case p := <-ports:
			return p, nil
		default:
			return nil, fmt.Errorf("test serial port unplugged")
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		defer close(done)
		b.Run(ctx)
	}()
	t.Cleanup(func() {
		cancel()
		_ = peer.conn.Close()
		select {
		case <-done:
		case <-time.After(2 * time.Second):
			t.Error("SerialBridge.Run did not stop")
		}
	})
	bridgeEventually(t, "serial connection", func() bool { return bridgeIsConnected(b) })
	return peer, ports
}

func TestSerialBridgeRecoversAtNewlineAfterOversizedInput(t *testing.T) {
	b := NewSerialBridge("test-pipe")
	lines := make(chan string, 4)
	errors := make(chan error, 4)
	b.onLine = func(line string) { lines <- line }
	b.onError = func(err error) { errors <- err }
	peer, _ := runBridgeTest(t, b)
	peer.write(t, strings.Repeat("x", bridgeLineMax+1)+`{"relay":2,"type":"hello"}`)
	select {
	case err := <-errors:
		if !strings.Contains(err.Error(), "exceeds 4096") {
			t.Fatalf("wrong overflow error: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("oversized serial line was not reported")
	}
	select {
	case line := <-lines:
		t.Fatalf("partial oversized line delivered: %q", line)
	default:
	}
	peer.write(t, "\n\nfirst\r\nsec")
	peer.write(t, "ond\n"+strings.Repeat("y", bridgeLineMax)+"\n")
	for _, want := range []string{"first", "second", strings.Repeat("y", bridgeLineMax)} {
		select {
		case got := <-lines:
			if got != want {
				t.Fatalf("line mismatch: got %d bytes, want %d", len(got), len(want))
			}
		case <-time.After(time.Second):
			t.Fatal("valid line after overflow was lost")
		}
	}
	select {
	case err := <-errors:
		t.Fatalf("unexpected extra error: %v", err)
	default:
	}
}

func TestSerialBridgeExpiresQueuedEffectsAndRejectsInvalidWrites(t *testing.T) {
	b := NewSerialBridge("test-pipe")
	if err := b.Send("offline"); err == nil {
		t.Fatal("disconnected send admitted")
	}
	errors := make(chan error, 4)
	b.onError = func(err error) { errors <- err }
	b.out <- bridgeWrite{line: "stale sound", at: time.Now().Add(-6 * time.Second)}
	b.out <- bridgeWrite{line: "fresh command", at: time.Now()}
	peer, _ := runBridgeTest(t, b)
	if got := peer.next(t); got != "fresh command" {
		t.Fatalf("expired effect replayed: %q", got)
	}
	select {
	case err := <-errors:
		if !strings.Contains(err.Error(), "expired") {
			t.Fatalf("wrong expiry error: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("queue expiry not reported")
	}
	for _, line := range []string{"", "a\nb", "a\rb", strings.Repeat("x", bridgeLineMax+1)} {
		if err := b.Send(line); err == nil {
			t.Errorf("invalid %d-byte command admitted", len(line))
		}
	}
	line := strings.Repeat("x", bridgeLineMax)
	if err := b.Send(line); err != nil {
		t.Fatal(err)
	}
	if got := peer.next(t); got != line {
		t.Fatal("maximum-length serial command was not preserved")
	}
	peer.quiet(t)
}

func TestSerialBridgeBoundedQueue(t *testing.T) {
	b := NewSerialBridge("test-pipe")
	b.setConnected(true)
	for i := 0; i < cap(b.out); i++ {
		if err := b.Send("queued"); err != nil {
			t.Fatal(err)
		}
	}
	if err := b.Send("overflow"); err == nil || !strings.Contains(err.Error(), "full") {
		t.Fatalf("full queue result: %v", err)
	}
}
