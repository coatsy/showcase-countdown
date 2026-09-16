package main

import (
	"encoding/json"
	"fmt"
	"strings"
	"sync"
	"testing"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

type relayTestMessage struct {
	topic   string
	payload []byte
}

type relayTestRig struct {
	fleet    *Fleet
	relay    *RadioRelay
	bridge   *SerialBridge
	peer     *bridgeTestPeer
	ports    chan bridgePort
	client   mqtt.Client
	mu       sync.Mutex
	messages []relayTestMessage
}

func relayWaitToken(t *testing.T, token mqtt.Token) {
	t.Helper()
	if !token.WaitTimeout(3 * time.Second) {
		t.Fatal("MQTT operation timed out")
	}
	if err := token.Error(); err != nil {
		t.Fatalf("MQTT operation: %v", err)
	}
}

func newRelayTestRig(t *testing.T) *relayTestRig {
	t.Helper()
	broker, err := startEmbeddedBroker("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := broker.Close(); err != nil {
			t.Errorf("broker cleanup: %v", err)
		}
	})
	listener, ok := broker.Listeners.Get("tcp4")
	if !ok {
		t.Fatal("embedded broker listener missing")
	}
	url := "tcp://" + listener.Address()
	rig := &relayTestRig{fleet: NewFleet(NewBus(256)), bridge: NewSerialBridge("test-pipe")}
	rig.relay = NewRadioRelay(rig.bridge, rig.fleet, NewPolicy(time.Time{}))
	rig.fleet.relay = rig.relay
	if err := rig.fleet.Connect(url); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { rig.fleet.client.Disconnect(100) })
	// Connect's OnConnect callback is asynchronous; wait for a SUBACK before
	// injecting frames rather than racing the initial subscription.
	relayWaitToken(t, rig.fleet.client.Subscribe(topicPrefix+"#", 1, rig.fleet.onMessage))
	rig.client = mqtt.NewClient(mqtt.NewClientOptions().AddBroker(url).
		SetClientID("relay-test-observer").SetAutoReconnect(false).SetOrderMatters(true))
	relayWaitToken(t, rig.client.Connect())
	t.Cleanup(func() { rig.client.Disconnect(100) })
	relayWaitToken(t, rig.client.Subscribe(topicPrefix+"#", 1, func(_ mqtt.Client, m mqtt.Message) {
		rig.mu.Lock()
		rig.messages = append(rig.messages, relayTestMessage{m.Topic(), append([]byte(nil), m.Payload()...)})
		rig.mu.Unlock()
	}))
	rig.peer, rig.ports = runBridgeTest(t, rig.bridge)
	return rig
}

func (r *relayTestRig) send(t *testing.T, value any) {
	t.Helper()
	raw, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	r.peer.write(t, string(raw)+"\n")
}

func (r *relayTestRig) hello(t *testing.T) {
	t.Helper()
	r.send(t, relayMessage{Relay: relayVersion, Type: "hello", BridgeID: "aabbcc"})
	var m relayMessage
	if err := json.Unmarshal([]byte(r.peer.next(t)), &m); err != nil {
		t.Fatal(err)
	}
	if m.Relay != relayVersion || m.Type != "time" || m.Epoch <= 0 {
		t.Fatalf("missing v2 clock handshake: %+v", m)
	}
}

func (r *relayTestRig) rx(t *testing.T, id, topic, payload string, session, seq uint32) {
	t.Helper()
	r.send(t, relayMessage{Relay: relayVersion, Type: "rx", ID: id, Topic: topic,
		Payload: json.RawMessage(payload), Session: &session, Seq: &seq})
}

func (r *relayTestRig) nextTX(t *testing.T, topic string) relayMessage {
	t.Helper()
	var m relayMessage
	if err := json.Unmarshal([]byte(r.peer.next(t)), &m); err != nil {
		t.Fatal(err)
	}
	if m.Relay != relayVersion || m.Type != "tx" || m.Topic != topic || m.Token == 0 {
		t.Fatalf("expected v2 tx %s, got %+v", topic, m)
	}
	return m
}

func (r *relayTestRig) ack(t *testing.T, m relayMessage, ok bool) {
	t.Helper()
	r.send(t, relayMessage{Relay: relayVersion, Type: "ack", ID: m.ID, Token: m.Token, OK: &ok})
	bridgeEventually(t, "acknowledgement admission", func() bool {
		r.relay.mu.Lock()
		defer r.relay.mu.Unlock()
		_, pending := r.relay.pending[m.Token]
		return !pending
	})
}

func (r *relayTestRig) register(t *testing.T, id string) {
	t.Helper()
	r.rx(t, id, "state", `{"online":true,"code":1234,"time_source":"bridge"}`, 7, 1)
	bridgeEventually(t, "radio registration", func() bool {
		d := r.fleet.Get(id)
		return d != nil && d.Online && d.Transport == "espnow"
	})
	r.ack(t, r.nextTX(t, "cmd/config"), true)
}

func (r *relayTestRig) mqttPublish(t *testing.T, topic, payload string) {
	t.Helper()
	relayWaitToken(t, r.client.Publish(topic, 1, false, []byte(payload)))
}

func (r *relayTestRig) observed(topic string) [][]byte {
	r.mu.Lock()
	defer r.mu.Unlock()
	var found [][]byte
	for _, m := range r.messages {
		if m.topic == topic {
			found = append(found, m.payload)
		}
	}
	return found
}

func relayEvents(bus *Bus, kind, device string) []Event {
	var found []Event
	for _, e := range bus.Recent(256) {
		if e.Kind == kind && (device == "" || e.Device == device) {
			found = append(found, e)
		}
	}
	return found
}

func TestRadioStatePreservesOTAMetadataAndExpiresReadiness(t *testing.T) {
	fleet := NewFleet(NewBus(32))
	now := time.Now()
	payload, err := json.Marshal(stateMsg{
		Online: true, Transport: "espnow", TimeSource: "ntp", RadioAt: now.UnixMilli(),
		IP: "192.168.8.42", OTA: true, Chip: "esp32", ImageMD5: "0123456789abcdef0123456789abcdef",
	})
	if err != nil {
		t.Fatal(err)
	}
	fleet.onState("abcdef", payload)
	d := fleet.Get("abcdef")
	if d == nil || !d.Online || !d.OTA || d.Transport != "espnow" || d.TimeSource != "ntp" ||
		d.IP != "192.168.8.42" || d.Chip != "esp32" || d.ImageMD5 != "0123456789abcdef0123456789abcdef" {
		t.Fatalf("merged radio/OTA metadata missing: %+v", d)
	}
	fleet.expireRadio(now.Add(radioOfflineAfter))
	if d = fleet.Get("abcdef"); d.Online || d.OTA {
		t.Fatalf("expired radio device still online or OTA-ready: %+v", d)
	}
}

func TestRadioRelayBrokerSerialACKAndDeliveryIDs(t *testing.T) {
	r := newRelayTestRig(t)
	r.hello(t)
	r.register(t, "abcdef")
	r.register(t, "012345")
	before := len(relayEvents(r.fleet.bus, "delivery", ""))
	ids := map[string]bool{}
	for i := 0; i < 2; i++ {
		if err := r.fleet.SendDisplay("abcdef", Display{Text: "same effect", Kind: "dm", TTL: 5}); err != nil {
			t.Fatal(err)
		}
		m := r.nextTX(t, "cmd/display")
		var payload map[string]any
		if err := json.Unmarshal(m.Payload, &payload); err != nil {
			t.Fatal(err)
		}
		id, _ := payload["_delivery_id"].(string)
		if id == "" || ids[id] {
			t.Fatalf("delivery ID missing or reused: %q", id)
		}
		ids[id] = true
		topic := devTopic("abcdef", "cmd", "display")
		bridgeEventually(t, "observer command", func() bool { return len(r.observed(topic)) == i+1 })
		var brokerPayload map[string]any
		if err := json.Unmarshal(r.observed(topic)[i], &brokerPayload); err != nil {
			t.Fatal(err)
		}
		if brokerPayload["_delivery_id"] != id || payload["text"] != "same effect" {
			t.Fatalf("MQTT payload changed in relay: %s -> %s", r.observed(topic)[i], m.Payload)
		}
		if got := len(relayEvents(r.fleet.bus, "delivery", "")); got != before+i {
			t.Fatal("queue admission incorrectly reported as acknowledged delivery")
		}
		r.ack(t, m, true)
	}
	bridgeEventually(t, "delivery events", func() bool {
		return len(relayEvents(r.fleet.bus, "delivery", "")) == before+2
	})
	for _, event := range relayEvents(r.fleet.bus, "delivery", "abcdef") {
		if !strings.Contains(event.Text, "admitted") || !strings.Contains(event.Text, "not physical") {
			t.Fatalf("ACK overstates execution: %s", event.Text)
		}
	}
	if err := r.fleet.BroadcastAudio(Audio{Jingle: "alarm"}); err != nil {
		t.Fatal(err)
	}
	var broadcastID string
	destinations := map[string]bool{}
	for i := 0; i < 2; i++ {
		m := r.nextTX(t, "cmd/audio")
		var payload map[string]any
		_ = json.Unmarshal(m.Payload, &payload)
		id, _ := payload["_delivery_id"].(string)
		if id == "" || (broadcastID != "" && id != broadcastID) || ids[id] {
			t.Fatalf("broadcast delivery ID not preserved: %s", m.Payload)
		}
		broadcastID = id
		destinations[m.ID] = true
		r.ack(t, m, true)
	}
	if len(destinations) != 2 || !destinations["abcdef"] || !destinations["012345"] {
		t.Fatalf("wrong broadcast destinations: %v", destinations)
	}
}

func TestRadioRelaySerialUplinkDedupAndButtonWaiter(t *testing.T) {
	r := newRelayTestRig(t)
	r.hello(t)
	r.register(t, "abcdef")
	d := r.fleet.Get("abcdef")
	if d.Code != 1234 || d.TimeSource != "bridge" || r.fleet.ByCode(1234).ID != "abcdef" {
		t.Fatalf("radio state not discoverable: %+v", d)
	}
	topic := topicPrefix + "dev/abcdef/state"
	bridgeEventually(t, "state at broker", func() bool { return len(r.observed(topic)) == 1 })
	var state stateMsg
	if err := json.Unmarshal(r.observed(topic)[0], &state); err != nil {
		t.Fatal(err)
	}
	if state.ID != "abcdef" || state.Transport != "espnow" || state.RadioAt <= 0 {
		t.Fatalf("server provenance missing: %+v", state)
	}
	result := make(chan ButtonEvent, 1)
	cancel := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		if ev, ok := r.fleet.WaitButton("abcdef", 3*time.Second, cancel); ok {
			result <- ev
		}
	}()
	t.Cleanup(func() { close(cancel); <-done })
	bridgeEventually(t, "button waiter", func() bool {
		r.fleet.mu.RLock()
		defer r.fleet.mu.RUnlock()
		return len(r.fleet.waiters["abcdef"]) == 1
	})
	r.rx(t, "abcdef", "event/button", `{"button":"A","action":"click"}`, 7, 2)
	select {
	case ev := <-result:
		if ev.Device != "abcdef" || ev.Button != "A" || ev.Action != "click" || ev.At.IsZero() {
			t.Fatalf("wrong button event: %+v", ev)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("serial button did not reach fleet waiter")
	}
	r.rx(t, "abcdef", "event/button", `{"button":"A","action":"click"}`, 7, 2)
	r.rx(t, "abcdef", "state", `{"online":true,"code":9999}`, 7, 1)
	// A subsequent unique event is a processing barrier for both duplicate frames.
	r.rx(t, "abcdef", "event/button", `{"button":"B","action":"double"}`, 7, 3)
	bridgeEventually(t, "unique button events", func() bool {
		return len(relayEvents(r.fleet.bus, "button", "abcdef")) == 2
	})
	buttonTopic := topicPrefix + "dev/abcdef/event/button"
	bridgeEventually(t, "broker button events", func() bool { return len(r.observed(buttonTopic)) == 2 })
	if len(r.observed(topic)) != 1 || r.fleet.Get("abcdef").Code != 1234 {
		t.Fatal("duplicate radio state was applied or republished")
	}
	r.rx(t, "abcdef", "state", `{"online":true,"code":4321}`, 8, 1)
	r.ack(t, r.nextTX(t, "cmd/config"), true)
	bridgeEventually(t, "new session sequence reuse", func() bool { return r.fleet.Get("abcdef").Code == 4321 })
}

func TestRadioRelayRejectsMalformedUplinks(t *testing.T) {
	r := newRelayTestRig(t)
	r.rx(t, "abcdef", "state", `{"online":true,"code":1234}`, 1, 1)
	bridgeEventually(t, "pre-handshake rejection", func() bool {
		return len(relayEvents(r.fleet.bus, "error", "")) == 1
	})
	r.hello(t)
	seq := uint32(2)
	for _, tc := range []struct{ name, line string }{
		{"broken JSON", `{"relay":2`},
		{"wrong version", `{"relay":1,"type":"hello","bridge_id":"aabbcc"}`},
		{"bad bridge ID", `{"relay":2,"type":"hello","bridge_id":"AABBCC"}`},
		{"unknown type", `{"relay":2,"type":"execute"}`},
		{"missing sequence", `{"relay":2,"type":"rx","id":"abcdef","session":1,"topic":"state","payload":{"online":true,"code":1234}}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			before := len(relayEvents(r.fleet.bus, "error", ""))
			r.peer.write(t, tc.line+"\n")
			bridgeEventually(t, "malformed envelope rejection", func() bool {
				return len(relayEvents(r.fleet.bus, "error", "")) == before+1
			})
		})
	}
	for _, tc := range []struct{ name, id, topic, payload string }{
		{"uppercase ID", "ABCDEF", "state", `{"online":true,"code":1234}`},
		{"virtual ID", "fake01", "state", `{"online":true,"code":1234}`},
		{"topic injection", "abc/ef", "state", `{"online":true,"code":1234}`},
		{"wildcard topic", "abcdef", "#", `{}`},
		{"command uplink", "abcdef", "cmd/audio", `{"jingle":"alarm"}`},
		{"array", "abcdef", "state", `[]`},
		{"null", "abcdef", "state", `null`},
		{"missing online", "abcdef", "state", `{"code":1234}`},
		{"missing claim code", "abcdef", "state", `{"online":true}`},
		{"invalid claim code", "abcdef", "state", `{"online":true,"code":10000}`},
		{"mismatched payload ID", "abcdef", "state", `{"online":true,"code":1234,"id":"012345"}`},
		{"invalid button", "abcdef", "event/button", `{"button":"C","action":"click"}`},
		{"invalid action", "abcdef", "event/button", `{"button":"A","action":"explode"}`},
		{"oversized payload", "abcdef", "state", `{"online":true,"code":1234,"padding":"` + strings.Repeat("x", relayPayloadMax) + `"}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			before := len(relayEvents(r.fleet.bus, "error", ""))
			seq++
			r.rx(t, tc.id, tc.topic, tc.payload, 1, seq)
			bridgeEventually(t, "invalid uplink rejection", func() bool {
				return len(relayEvents(r.fleet.bus, "error", "")) == before+1
			})
		})
	}
	if len(r.fleet.Snapshot()) != 0 {
		t.Fatalf("invalid uplinks created devices: %+v", r.fleet.Snapshot())
	}
	r.mu.Lock()
	count := len(r.messages)
	r.mu.Unlock()
	if count != 0 {
		t.Fatalf("invalid uplinks reached broker: %d", count)
	}
	// Rejected messages must not poison the receipt key.
	r.rx(t, "abcdef", "state", `{"online":true,"code":1234}`, 1, seq)
	r.ack(t, r.nextTX(t, "cmd/config"), true)
}

func TestRadioRelayHeartbeatExpiryLateLWTAndMQTTRecovery(t *testing.T) {
	r := newRelayTestRig(t)
	r.hello(t)
	r.register(t, "abcdef")
	topic := topicPrefix + "dev/abcdef/state"
	r.mqttPublish(t, topic, `{"online":false}`)
	// Same-publisher messages are ordered; command reflection is a barrier.
	r.mqttPublish(t, devTopic("abcdef", "cmd", "display"), `{"text":"LWT barrier"}`)
	r.ack(t, r.nextTX(t, "cmd/display"), true)
	bridgeEventually(t, "LWT barrier reflected", func() bool {
		d := r.fleet.Get("abcdef")
		return d.Screen != nil && d.Screen.Text == "LWT barrier"
	})
	if d := r.fleet.Get("abcdef"); !d.Online || d.Transport != "espnow" {
		t.Fatalf("late direct LWT hid live radio: %+v", d)
	}
	d := r.fleet.Get("abcdef")
	r.fleet.expireRadio(d.radioSeen.Add(radioOfflineAfter - time.Millisecond))
	if !r.fleet.Get("abcdef").Online {
		t.Fatal("radio expired before heartbeat deadline")
	}
	r.fleet.expireRadio(d.radioSeen.Add(radioOfflineAfter))
	if r.fleet.Get("abcdef").Online {
		t.Fatal("radio did not expire at heartbeat deadline")
	}
	r.mqttPublish(t, topic, fmt.Sprintf(`{"online":true,"code":1234,"transport":"espnow","_radio_at_ms":%d}`,
		time.Now().Add(-radioOfflineAfter-time.Second).UnixMilli()))
	r.mqttPublish(t, devTopic("abcdef", "cmd", "display"), `{"text":"stale state barrier"}`)
	bridgeEventually(t, "stale state barrier", func() bool {
		return r.fleet.Get("abcdef").Screen.Text == "stale state barrier"
	})
	if r.fleet.Get("abcdef").Online {
		t.Fatal("stale retained-style radio state resurrected device")
	}
	r.peer.quiet(t)
	r.mqttPublish(t, topic, `{"online":true,"code":1234,"ntp":true}`)
	bridgeEventually(t, "direct MQTT recovery", func() bool {
		d := r.fleet.Get("abcdef")
		return d.Online && d.Transport == "mqtt" && d.TimeSource == "ntp" && d.radioSeen.IsZero()
	})
	r.fleet.expireRadio(time.Now().Add(2 * radioOfflineAfter))
	if !r.fleet.Get("abcdef").Online {
		t.Fatal("radio watchdog expired a recovered MQTT device")
	}
	if err := r.fleet.SendAudio("abcdef", Audio{Jingle: "alarm"}); err != nil {
		t.Fatal(err)
	}
	bridgeEventually(t, "MQTT-only audio reflection", func() bool { return r.fleet.Get("abcdef").Audio != nil })
	r.peer.quiet(t)
	r.mqttPublish(t, topic, `{"online":false}`)
	bridgeEventually(t, "direct LWT after recovery", func() bool { return !r.fleet.Get("abcdef").Online })
}

func TestRadioRelayReconnectDropsPendingAndRequiresNewHello(t *testing.T) {
	r := newRelayTestRig(t)
	r.hello(t)
	r.register(t, "abcdef")
	if err := r.fleet.SendAudio("abcdef", Audio{Jingle: "alarm"}); err != nil {
		t.Fatal(err)
	}
	lost := r.nextTX(t, "cmd/audio")
	_ = r.peer.conn.Close()
	bridgeEventually(t, "serial disconnect", func() bool {
		return !bridgeIsConnected(r.bridge) && !r.relay.ready()
	})
	bridgeEventually(t, "pending command loss", func() bool {
		r.relay.mu.Lock()
		defer r.relay.mu.Unlock()
		return len(r.relay.pending) == 0
	})
	if !strings.Contains(fmt.Sprint(relayEvents(r.fleet.bus, "error", "abcdef")), "not replaying") {
		t.Fatal("disconnect did not explain command loss")
	}
	if err := r.bridge.Send("offline effect"); err == nil {
		t.Fatal("disconnected effect was queued")
	}
	port, peer := newBridgeTestPipe(t)
	r.ports <- port
	r.peer = peer
	bridgeEventually(t, "serial reconnect", func() bool { return bridgeIsConnected(r.bridge) })
	if r.relay.ready() {
		t.Fatal("serial reconnect reused stale handshake")
	}
	if err := r.fleet.SendDisplay("abcdef", Display{Text: "before hello"}); err != nil {
		t.Fatal(err)
	}
	bridgeEventually(t, "pre-handshake command consumed", func() bool {
		d := r.fleet.Get("abcdef")
		return d.Screen != nil && d.Screen.Text == "before hello"
	})
	peer.quiet(t)
	r.hello(t)
	peer.quiet(t)
	before := len(relayEvents(r.fleet.bus, "error", "bridge"))
	ok := true
	r.send(t, relayMessage{Relay: relayVersion, Type: "ack", ID: lost.ID, Token: lost.Token, OK: &ok})
	bridgeEventually(t, "old acknowledgement rejected", func() bool {
		return len(relayEvents(r.fleet.bus, "error", "bridge")) == before+1
	})
	if err := r.fleet.SendLed("abcdef", Led{Color: "#FF0000", Mode: "solid"}); err != nil {
		t.Fatal(err)
	}
	fresh := r.nextTX(t, "cmd/led")
	if fresh.Token == lost.Token {
		t.Fatal("new command reused lost token")
	}
	r.ack(t, fresh, true)
}

func TestRadioRelaySameSessionReconnectRestoresCurrentConfigOnly(t *testing.T) {
	for _, outage := range []string{"serial disconnect", "expired hello"} {
		t.Run(outage, func(t *testing.T) {
			r := newRelayTestRig(t)
			r.hello(t)
			r.register(t, "abcdef")
			if outage == "serial disconnect" {
				_ = r.peer.conn.Close()
				bridgeEventually(t, "serial disconnect and handshake reset", func() bool {
					return !bridgeIsConnected(r.bridge) && !r.relay.ready()
				})
			} else {
				r.relay.mu.Lock()
				r.relay.lastHello = time.Now().Add(-31 * time.Second)
				r.relay.mu.Unlock()
			}

			r.relay.policy.SetManualLock(true)
			team := "Reconnected team"
			if err := r.fleet.SendConfig("abcdef", Config{Team: &team}); err != nil {
				t.Fatal(err)
			}
			if err := r.fleet.SendAudio("abcdef", Audio{Jingle: "alarm"}); err != nil {
				t.Fatal(err)
			}
			if err := r.fleet.SendDisplay("abcdef", Display{Text: "missed during outage"}); err != nil {
				t.Fatal(err)
			}
			// Ordered MQTT reflection proves all outage commands were consumed
			// before the next hello makes serial forwarding ready again.
			bridgeEventually(t, "outage commands reflected", func() bool {
				d := r.fleet.Get("abcdef")
				return d.Audio != nil && d.Screen != nil && d.Screen.Text == "missed during outage"
			})
			if d := r.fleet.Get("abcdef"); !d.Online || d.Transport != "espnow" || d.Locked {
				t.Fatalf("expected still-online radio with old lock state: %+v", d)
			}
			if outage == "serial disconnect" {
				port, peer := newBridgeTestPipe(t)
				r.ports <- port
				r.peer = peer
				bridgeEventually(t, "serial reconnect", func() bool { return bridgeIsConnected(r.bridge) })
			}
			r.hello(t)
			r.peer.quiet(t)

			// Same radio boot/session, but a fresh heartbeat sequence.
			r.rx(t, "abcdef", "state", `{"online":true,"code":1234}`, 7, 2)
			m := r.nextTX(t, "cmd/config")
			var config Config
			if err := json.Unmarshal(m.Payload, &config); err != nil {
				t.Fatal(err)
			}
			if m.ID != "abcdef" || config.Team == nil || *config.Team != team ||
				config.Locked == nil || !*config.Locked {
				t.Fatalf("same-session registration did not restore current team and policy lock: %s", m.Payload)
			}
			r.ack(t, m, true)
			bridgeEventually(t, "restored config reflected", func() bool {
				d := r.fleet.Get("abcdef")
				return d.Online && d.Transport == "espnow" && d.Team == team && d.Locked
			})
			r.peer.quiet(t)
		})
	}
}

func TestRadioRelayForwardValidationAndRetention(t *testing.T) {
	b := NewSerialBridge("queue-only")
	b.setConnected(true)
	f := NewFleet(NewBus(128))
	r := NewRadioRelay(b, f, NewPolicy(time.Time{}))
	r.lastHello = time.Now()
	for _, d := range []Device{
		{ID: "abcdef", Online: true, Transport: "espnow"},
		{ID: "012345", Online: true, Transport: "mqtt"},
		{ID: "123456", Online: false, Transport: "espnow"},
		{ID: "fake01", Online: true, Virtual: true, Transport: "espnow"},
	} {
		copy := d
		f.devices[d.ID] = &copy
	}
	for _, topic := range []string{
		"showcase/dev/ABCDEF/cmd/audio", "showcase/dev/abc/ef/cmd/audio",
		"showcase/dev/abcdef/cmd/reboot", "showcase/dev/abcdef/cmd/audio/extra",
		"other/dev/abcdef/cmd/audio", "showcase/dev/+/cmd/audio",
		"showcase/dev/012345/cmd/audio", "showcase/dev/123456/cmd/audio",
		"showcase/dev/fake01/cmd/audio", "showcase/dev/999999/cmd/audio",
	} {
		r.Forward(topic, []byte(`{}`), false)
	}
	for _, payload := range []string{`[]`, `null`, `{`, `{"padding":"` + strings.Repeat("x", relayPayloadMax) + `"}`} {
		r.Forward(devTopic("abcdef", "cmd", "audio"), []byte(payload), false)
	}
	for _, command := range []string{"audio", "display", "led"} {
		r.Forward(devTopic("abcdef", "cmd", command), []byte(`{}`), true)
	}
	if len(b.out) != 0 || len(r.pending) != 0 {
		t.Fatal("invalid, offline, MQTT-only or retained effect was queued")
	}
	payload := `{"_delivery_id":"external-command","locked":true}`
	r.Forward("showcase/all/cmd/config", []byte(payload), true)
	if len(b.out) != 1 || len(r.pending) != 1 {
		t.Fatalf("retained config did not target exactly one radio device: writes=%d pending=%d", len(b.out), len(r.pending))
	}
	var m relayMessage
	if err := json.Unmarshal([]byte((<-b.out).line), &m); err != nil {
		t.Fatal(err)
	}
	if m.ID != "abcdef" || m.Topic != "cmd/config" || string(m.Payload) != payload {
		t.Fatalf("config target or delivery ID changed: %+v", m)
	}
}
