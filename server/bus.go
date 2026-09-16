package main

import (
	"sync"
	"time"
)

// Event is one line in the room's live log.
type Event struct {
	At     time.Time `json:"at"`
	Kind   string    `json:"kind"` // presence, button, display, audio, led, shout, dm, organiser, lock, claim, error
	Device string    `json:"device,omitempty"`
	Team   string    `json:"team,omitempty"`
	Text   string    `json:"text"`
	id     uint64
}

// Bus keeps a ring of recent events and fans new ones out to subscribers.
type Bus struct {
	mu   sync.Mutex
	ring []Event
	subs map[chan Event]struct{}
	max  int
	next uint64
}

func NewBus(max int) *Bus {
	return &Bus{subs: map[chan Event]struct{}{}, max: max}
}

func (b *Bus) Emit(e Event) {
	if e.At.IsZero() {
		e.At = time.Now()
	}
	b.mu.Lock()
	b.next++
	e.id = b.next
	b.ring = append(b.ring, e)
	if len(b.ring) > b.max {
		b.ring = b.ring[len(b.ring)-b.max:]
	}
	for ch := range b.subs {
		select {
		case ch <- e:
		default: // a slow dashboard tab must not stall the room
		}
	}
	b.mu.Unlock()
}

func (b *Bus) RecentAfter(id uint64, n int) []Event {
	b.mu.Lock()
	defer b.mu.Unlock()
	start := 0
	if id > 0 && len(b.ring) > 0 && id <= b.ring[len(b.ring)-1].id {
		for start < len(b.ring) && b.ring[start].id <= id {
			start++
		}
	}
	if len(b.ring)-start > n {
		start = len(b.ring) - n
	}
	out := make([]Event, len(b.ring)-start)
	copy(out, b.ring[start:])
	return out
}

func (b *Bus) Recent(n int) []Event {
	b.mu.Lock()
	defer b.mu.Unlock()
	if n > len(b.ring) {
		n = len(b.ring)
	}
	out := make([]Event, n)
	copy(out, b.ring[len(b.ring)-n:])
	return out
}

func (b *Bus) Subscribe() (chan Event, func()) {
	ch := make(chan Event, 64)
	b.mu.Lock()
	b.subs[ch] = struct{}{}
	b.mu.Unlock()
	return ch, func() {
		b.mu.Lock()
		delete(b.subs, ch)
		b.mu.Unlock()
	}
}
