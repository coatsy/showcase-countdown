package main

import (
	"context"
	"fmt"
	"io"
	"log"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// The ESP-NOW bridge stick relays "lock", "unlock" and "fire <epoch>" as
// broadcast frames (docs/messaging.md). It normally listens on MQTT topic
// showcase/bridge/cmd, powered from the router's USB port. A USB serial link
// is an optional second path for hosts whose kernel can drive the stick's
// USB-serial chip.

const bridgeCmdTopic = topicPrefix + "bridge/cmd"

// bridgeSend fans one command out over every available path.
func (a *App) bridgeSend(line string) {
	// Lock state is retained so a bridge that reconnects catches up; a fire
	// signal is only meaningful at the instant it is sent.
	retain := !strings.HasPrefix(line, "fire")
	if a.fleet.Connected() {
		if err := a.fleet.PublishRaw(bridgeCmdTopic, line, retain); err != nil {
			log.Printf("bridge: mqtt publish %q: %v", line, err)
		}
	}
	if a.bridge != nil {
		if err := a.bridge.Send(line); err != nil {
			log.Printf("bridge serial: %v", err)
			a.bus.Emit(Event{Kind: "error", Device: "bridge", Text: err.Error()})
		}
	}
}

// armFire sends the fire signal three seconds before the event so sticks
// that lost their clock still start on time. A stick ignores a fire whose
// epoch does not match its built-in target.
func (a *App) armFire() {
	if a.epoch.IsZero() {
		return
	}
	go func() {
		wait := time.Until(a.epoch.Add(-3 * time.Second))
		if wait < 0 {
			return
		}
		time.Sleep(wait)
		line := fmt.Sprintf("fire %d", a.epoch.Unix())
		a.bridgeSend(line)
		a.bus.Emit(Event{Kind: "lock", Text: "fire signal sent to the bridge"})
		log.Printf("bridge: %s", line)
	}()
}

const bridgeLineMax = 4096

type bridgePort interface {
	io.ReadWriteCloser
	SetReadTimeout(time.Duration) error
}

type bridgeWrite struct {
	line string
	at   time.Time
}

// A disconnected bridge must not replay old sounds and messages after USB returns.
type SerialBridge struct {
	mu           sync.Mutex
	name         string
	connected    bool
	out          chan bridgeWrite
	onLine       func(string)
	onConnection func(bool)
	onError      func(error)
	open         func() (bridgePort, error)
}

func NewSerialBridge(name string) *SerialBridge {
	b := &SerialBridge{name: name, out: make(chan bridgeWrite, 64)}
	b.open = func() (bridgePort, error) {
		path := name
		if name == "auto" {
			ports, err := enumerator.GetDetailedPortsList()
			if err != nil {
				return nil, err
			}
			path = ""
			for _, p := range ports {
				if p.IsUSB && strings.EqualFold(p.VID, "0403") && strings.EqualFold(p.PID, "6001") {
					if path != "" {
						return nil, fmt.Errorf("multiple FTDI bridges found; set BRIDGE_PORT explicitly")
					}
					path = p.Name
				}
			}
			if path == "" {
				return nil, fmt.Errorf("no FTDI bridge found")
			}
		}
		return serial.Open(path, &serial.Mode{BaudRate: 115200})
	}
	return b
}

func (b *SerialBridge) report(err error) {
	log.Printf("bridge serial: %v", err)
	if b.onError != nil {
		b.onError(err)
	}
}

func (b *SerialBridge) setConnected(on bool) {
	b.mu.Lock()
	b.connected = on
	b.mu.Unlock()
	if b.onConnection != nil {
		b.onConnection(on)
	}
}

func (b *SerialBridge) Run(ctx context.Context) {
	for ctx.Err() == nil {
		port, err := b.open()
		if err == nil {
			err = port.SetReadTimeout(200 * time.Millisecond)
			if err == nil {
				b.setConnected(true)
				log.Printf("bridge: serial connected (%s)", b.name)
				err = b.serve(ctx, port)
				b.setConnected(false)
			}
			_ = port.Close()
		}
		if err != nil && ctx.Err() == nil {
			b.report(err)
		}
		for len(b.out) > 0 {
			<-b.out
			b.report(fmt.Errorf("discarded queued command after serial disconnect"))
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(2 * time.Second):
		}
	}
}

func (b *SerialBridge) serve(ctx context.Context, port bridgePort) error {
	buf := make([]byte, 256)
	line := ""
	discard := false
	for ctx.Err() == nil {
		for i := 0; i < 8; i++ {
			select {
			case item := <-b.out:
				if time.Since(item.at) > 5*time.Second {
					b.report(fmt.Errorf("discarded expired serial command"))
					continue
				}
				data := []byte(item.line + "\n")
				n, err := port.Write(data)
				if err != nil {
					return fmt.Errorf("write: %w", err)
				}
				if n != len(data) {
					return io.ErrShortWrite
				}
			default:
				i = 8
			}
		}
		n, err := port.Read(buf)
		if err != nil {
			return fmt.Errorf("read: %w", err)
		}
		for _, c := range buf[:n] {
			if c == '\n' {
				if !discard && line != "" {
					if b.onLine != nil {
						b.onLine(line)
					} else {
						log.Printf("bridge serial: %s", line)
					}
				}
				line = ""
				discard = false
			} else if c != '\r' && !discard {
				if len(line) == bridgeLineMax {
					discard = true
					line = ""
					b.report(fmt.Errorf("serial line exceeds %d bytes", bridgeLineMax))
				} else {
					line += string(c)
				}
			}
		}
	}
	return nil
}

func (b *SerialBridge) Send(line string) error {
	if line == "" || len(line) > bridgeLineMax || strings.ContainsAny(line, "\r\n") {
		return fmt.Errorf("invalid serial command length or newline")
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if !b.connected {
		return fmt.Errorf("serial bridge is disconnected")
	}
	select {
	case b.out <- bridgeWrite{line: line, at: time.Now()}:
		return nil
	default:
		return fmt.Errorf("serial bridge command queue is full")
	}
}
