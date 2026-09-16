package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"strings"
	"sync"
	"time"
)

const (
	relayVersion      = 2
	relayPayloadMax   = 767
	radioOfflineAfter = 45 * time.Second
)

type relayMessage struct {
	Relay    int             `json:"relay"`
	Type     string          `json:"type"`
	ID       string          `json:"id,omitempty"`
	BridgeID string          `json:"bridge_id,omitempty"`
	Topic    string          `json:"topic,omitempty"`
	Payload  json.RawMessage `json:"payload,omitempty"`
	Session  *uint32         `json:"session,omitempty"`
	Seq      *uint32         `json:"seq,omitempty"`
	Token    uint32          `json:"token,omitempty"`
	OK       *bool           `json:"ok,omitempty"`
	Error    string          `json:"error,omitempty"`
	Epoch    int64           `json:"epoch,omitempty"`
}

type relayReceiptKey struct {
	id           string
	session, seq uint32
}

type pendingRadioCommand struct {
	id string
	at time.Time
}

// RadioRelay is a topic bridge, not a TCP tunnel. ACK means command admission,
// never proof that a physical speaker, display or LED produced the output.
type RadioRelay struct {
	mu        sync.Mutex
	serial    *SerialBridge
	fleet     *Fleet
	policy    *Policy
	lastHello time.Time
	token     uint32
	pending   map[uint32]pendingRadioCommand
	seen      map[relayReceiptKey]time.Time
	sessions  map[string]uint32
}

func NewRadioRelay(serial *SerialBridge, fleet *Fleet, policy *Policy) *RadioRelay {
	r := &RadioRelay{
		serial: serial, fleet: fleet, policy: policy,
		pending:  make(map[uint32]pendingRadioCommand),
		seen:     make(map[relayReceiptKey]time.Time),
		sessions: make(map[string]uint32),
	}
	serial.onLine = r.HandleLine
	serial.onConnection = func(up bool) {
		if !up {
			r.mu.Lock()
			r.lastHello = time.Time{}
			r.sessions = make(map[string]uint32)
			for token, p := range r.pending {
				r.failure(p.id, fmt.Errorf("command %d lost its serial connection; not replaying", token))
				delete(r.pending, token)
			}
			r.mu.Unlock()
			fleet.setSerialBridge(false)
		}
	}
	serial.onError = func(err error) { r.failure("bridge", err) }
	return r
}

func (r *RadioRelay) failure(id string, err error) {
	log.Printf("relay %s: %v", id, err)
	r.fleet.bus.Emit(Event{Kind: "error", Device: id, Text: "ESP-NOW relay: " + err.Error()})
}

func physicalID(id string) bool {
	if len(id) != 6 {
		return false
	}
	for _, c := range id {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func relayCommand(topic string) (string, string, bool) {
	p := strings.Split(topic, "/")
	id, command := "", ""
	if len(p) == 5 && p[0] == "showcase" && p[1] == "dev" && p[3] == "cmd" && physicalID(p[2]) {
		id, command = p[2], p[4]
	} else if len(p) == 4 && p[0] == "showcase" && p[1] == "all" && p[2] == "cmd" {
		id, command = "all", p[3]
	}
	switch command {
	case "display", "audio", "led", "config":
		return id, "cmd/" + command, true
	default:
		return "", "", false
	}
}

func relayObject(payload []byte) (map[string]json.RawMessage, error) {
	if len(payload) == 0 || len(payload) > relayPayloadMax {
		return nil, fmt.Errorf("payload must be 1-%d bytes", relayPayloadMax)
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(payload, &obj); err != nil || obj == nil {
		return nil, fmt.Errorf("payload must be a JSON object")
	}
	return obj, nil
}

func (r *RadioRelay) ready() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return !r.lastHello.IsZero() && time.Since(r.lastHello) < 30*time.Second
}

func (r *RadioRelay) write(m relayMessage) error {
	m.Relay = relayVersion
	raw, err := json.Marshal(m)
	if err != nil {
		return err
	}
	return r.serial.Send(string(raw))
}

func (r *RadioRelay) sendCommand(id, topic string, payload []byte) error {
	if !r.ready() {
		return fmt.Errorf("v2 serial relay is not ready")
	}
	if !physicalID(id) {
		return fmt.Errorf("invalid physical destination")
	}
	if _, err := relayObject(payload); err != nil {
		return err
	}
	r.mu.Lock()
	if len(r.pending) >= 64 {
		r.mu.Unlock()
		return fmt.Errorf("radio acknowledgement queue is full")
	}
	r.token++
	if r.token == 0 {
		r.token++
	}
	token := r.token
	r.pending[token] = pendingRadioCommand{id: id, at: time.Now()}
	r.mu.Unlock()
	err := r.write(relayMessage{Type: "tx", ID: id, Topic: topic, Payload: payload, Token: token})
	if err != nil {
		r.mu.Lock()
		delete(r.pending, token)
		r.mu.Unlock()
	}
	return err
}

func (r *RadioRelay) Forward(topic string, payload []byte, retained bool) {
	id, command, ok := relayCommand(topic)
	if !ok || !r.ready() {
		return
	}
	// Retained audio/display would replay after reconnect; only config is durable.
	if retained && command != "cmd/config" {
		return
	}
	ids := []string{id}
	if id == "all" {
		ids = nil
		for _, d := range r.fleet.Snapshot() {
			if !d.Virtual && d.Online && physicalID(d.ID) {
				ids = append(ids, d.ID)
			}
		}
	}
	for _, target := range ids {
		d := r.fleet.Get(target)
		if d == nil || d.Virtual || !d.Online || d.Transport != "espnow" {
			continue
		}
		if err := r.sendCommand(target, command, payload); err != nil {
			r.failure(target, err)
		}
	}
}

func (r *RadioRelay) HandleLine(line string) {
	if !strings.HasPrefix(strings.TrimSpace(line), "{") {
		log.Printf("bridge serial: %s", line)
		return
	}
	var m relayMessage
	if len(line) > bridgeLineMax || json.Unmarshal([]byte(line), &m) != nil || m.Relay != relayVersion {
		r.failure("bridge", fmt.Errorf("invalid serial relay envelope"))
		return
	}
	switch m.Type {
	case "hello":
		if !physicalID(m.BridgeID) {
			r.failure("bridge", fmt.Errorf("invalid bridge identity"))
			return
		}
		r.mu.Lock()
		first := r.lastHello.IsZero() || time.Since(r.lastHello) >= 30*time.Second
		if first {
			r.sessions = make(map[string]uint32)
		}
		r.lastHello = time.Now()
		r.mu.Unlock()
		r.fleet.setSerialBridge(true)
		if first {
			if err := r.write(relayMessage{Type: "time", Epoch: time.Now().Unix()}); err != nil {
				r.failure("bridge", err)
			}
		}
	case "rx":
		if !r.ready() {
			r.failure(m.ID, fmt.Errorf("uplink before relay handshake"))
			return
		}
		if err := r.receive(m); err != nil {
			r.failure(m.ID, err)
		}
	case "ack":
		r.mu.Lock()
		p, ok := r.pending[m.Token]
		if ok && m.ID == p.id && m.OK != nil {
			delete(r.pending, m.Token)
		}
		r.mu.Unlock()
		if !ok || m.ID != p.id || m.OK == nil {
			r.failure("bridge", fmt.Errorf("unknown or malformed acknowledgement"))
			return
		}
		if !*m.OK {
			r.failure(m.ID, fmt.Errorf("command %d rejected: %.160s", m.Token, m.Error))
		} else {
			r.fleet.bus.Emit(Event{Kind: "delivery", Device: m.ID,
				Text: fmt.Sprintf("ESP-NOW command %d admitted (not physical-output confirmation)", m.Token)})
		}
	default:
		r.failure("bridge", fmt.Errorf("unknown serial relay message type"))
	}
}

func (r *RadioRelay) receive(m relayMessage) error {
	if !physicalID(m.ID) || m.Session == nil || m.Seq == nil {
		return fmt.Errorf("uplink requires device, session and sequence")
	}
	obj, err := relayObject(m.Payload)
	if err != nil {
		return err
	}
	key := relayReceiptKey{m.ID, *m.Session, *m.Seq}
	r.mu.Lock()
	_, duplicate := r.seen[key]
	session, knownSession := r.sessions[m.ID]
	r.mu.Unlock()
	if duplicate {
		return nil
	}
	switch m.Topic {
	case "state":
		var st stateMsg
		if json.Unmarshal(m.Payload, &st) != nil || obj["online"] == nil ||
			(st.ID != "" && st.ID != m.ID) || (st.Online && (st.Code < 1 || st.Code > 9999)) {
			return fmt.Errorf("invalid radio state")
		}
		obj["transport"] = json.RawMessage(`"espnow"`)
		obj["id"], _ = json.Marshal(m.ID)
		obj["_radio_at_ms"], _ = json.Marshal(time.Now().UnixMilli())
	case "event/button":
		var event ButtonEvent
		if json.Unmarshal(m.Payload, &event) != nil ||
			(event.Button != "A" && event.Button != "B") ||
			(event.Action != "click" && event.Action != "double" && event.Action != "press") {
			return fmt.Errorf("invalid radio button event")
		}
	default:
		return fmt.Errorf("uplink topic is not permitted")
	}
	payload, err := json.Marshal(obj)
	if err != nil {
		return err
	}
	previous := r.fleet.Get(m.ID)
	if err := r.fleet.publishRadio(topicPrefix+"dev/"+m.ID+"/"+m.Topic, payload, m.Topic == "state"); err != nil {
		return err
	}
	r.mu.Lock()
	if len(r.seen) >= 256 {
		var oldest relayReceiptKey
		oldestAt := time.Now()
		for k, at := range r.seen {
			if at.Before(oldestAt) {
				oldest, oldestAt = k, at
			}
		}
		delete(r.seen, oldest)
	}
	r.seen[key] = time.Now()
	if m.Topic == "state" {
		r.sessions[m.ID] = *m.Session
	}
	r.mu.Unlock()
	if m.Topic == "state" && (!knownSession || session != *m.Session || previous == nil || previous.Transport != "espnow") {
		team := ""
		if previous != nil {
			team = previous.Team
		}
		locked := r.policy.Locked()
		c := Config{Locked: &locked}
		if team != "" {
			c.Team = &team
		}
		// Re-send only durable configuration on registration, never stale effects.
		if err := r.fleet.SendConfig(m.ID, c); err != nil {
			return fmt.Errorf("restore config: %w", err)
		}
	}
	return nil
}

func (r *RadioRelay) maintain(now time.Time) {
	r.mu.Lock()
	for key, at := range r.seen {
		if now.Sub(at) > time.Minute {
			delete(r.seen, key)
		}
	}
	for token, p := range r.pending {
		if now.Sub(p.at) > 10*time.Second {
			r.failure(p.id, fmt.Errorf("command %d acknowledgement timed out; not replaying", token))
			delete(r.pending, token)
		}
	}
	expired := !r.lastHello.IsZero() && now.Sub(r.lastHello) >= 30*time.Second
	r.mu.Unlock()
	if expired {
		r.fleet.setSerialBridge(false)
	}
	r.fleet.expireRadio(now)
	if r.ready() {
		if err := r.write(relayMessage{Type: "time", Epoch: now.Unix()}); err != nil {
			r.failure("bridge", err)
		}
	}
}

func (r *RadioRelay) Run(ctx context.Context) {
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			r.maintain(now)
		}
	}
}
