package main

import (
	"encoding/json"
	"fmt"
	"log"
	"os"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

// Topic scheme, mirrored from docs/messaging.md.
const topicPrefix = "showcase/"

// Device is the server's view of one stick, real or virtual. Screen and LED
// are derived from the commands the server has seen, so the dashboard can draw
// what a stick should be showing without the stick reporting pixels.
type Device struct {
	ID       string    `json:"id"`
	Virtual  bool      `json:"virtual"`
	Online   bool      `json:"online"`
	Code     int       `json:"code"`
	Team     string    `json:"team"`
	Voice    string    `json:"voice"`
	Battery  int       `json:"battery"`
	NTP      bool      `json:"ntp"`
	Fired    bool      `json:"fired"`
	Heap     int       `json:"heap"`
	Uptime   int       `json:"uptime_s"`
	FW       string    `json:"fw"`
	IP       string    `json:"ip"`
	OTA      bool      `json:"ota"`
	Chip     string    `json:"chip"`
	ImageMD5 string    `json:"image_md5"`
	Locked   bool      `json:"locked"`
	LastSeen time.Time `json:"last_seen"`

	Screen *Display   `json:"screen,omitempty"`
	LED    *Led       `json:"led,omitempty"`
	Audio  *AudioNote `json:"audio,omitempty"`
}

// Display mirrors showcase/dev/<id>/cmd/display.
type Display struct {
	Text     string    `json:"text"`
	From     string    `json:"from,omitempty"`
	Kind     string    `json:"kind"`
	TTL      int       `json:"ttl_s"`
	Priority int       `json:"priority"`
	Until    time.Time `json:"until,omitempty"`
}

// Audio mirrors showcase/dev/<id>/cmd/audio.
type Audio struct {
	Jingle string `json:"jingle,omitempty"`
	Notes  string `json:"notes,omitempty"`
	Volume int    `json:"volume,omitempty"`
}

// AudioNote is what the dashboard shows for the last audio command.
type AudioNote struct {
	Label string    `json:"label"`
	At    time.Time `json:"at"`
}

// Led mirrors showcase/dev/<id>/cmd/led.
type Led struct {
	Color    string    `json:"color"`
	Mode     string    `json:"mode"`
	PeriodMs int       `json:"period_ms,omitempty"`
	TTL      int       `json:"ttl_s"`
	Until    time.Time `json:"until,omitempty"`
}

// Config mirrors the retained showcase/dev/<id>/cmd/config.
type Config struct {
	Team   *string `json:"team,omitempty"`
	Locked *bool   `json:"locked,omitempty"`
}

type stateMsg struct {
	Online  bool   `json:"online"`
	ID      string `json:"id"`
	Voice   string `json:"voice"`
	Battery int    `json:"battery"`
	NTP     bool   `json:"ntp"`
	Fired   bool   `json:"fired"`
	Code    int    `json:"code"`
	Team    string `json:"team"`
	Uptime  int    `json:"uptime_s"`
	Heap    int    `json:"heap"`
	FW      string `json:"fw"`
	IP      string `json:"ip"`
	OTA     bool   `json:"ota"`
	Chip    string `json:"chip"`
	ImageMD5 string `json:"image_md5"`
}

// ButtonEvent mirrors showcase/dev/<id>/event/button.
type ButtonEvent struct {
	Device string    `json:"device"`
	Button string    `json:"button"`
	Action string    `json:"action"`
	At     time.Time `json:"at"`
}

// Fleet is the device registry plus the broker connection.
type Fleet struct {
	mu         sync.RWMutex
	devices    map[string]*Device
	waiters    map[string][]chan ButtonEvent
	bus        *Bus
	client     mqtt.Client
	bridgeText string

	onTeamsChanged func() // set by main to persist team names
}

// Team names are the one piece of state that must survive a restart of the
// router, whose broker restarts with the server. They are written to a small
// JSON file on every change and republished as retained config on connect.

type teamsFile struct {
	Teams map[string]string `json:"teams"` // device id -> team name
}

func (f *Fleet) SaveTeams(path string) {
	if path == "" {
		return
	}
	f.mu.RLock()
	tf := teamsFile{Teams: map[string]string{}}
	for id, d := range f.devices {
		if d.Team != "" {
			tf.Teams[id] = d.Team
		}
	}
	f.mu.RUnlock()
	b, _ := json.MarshalIndent(tf, "", "  ")
	if err := os.WriteFile(path, b, 0o644); err != nil {
		log.Printf("teams: save %s: %v", path, err)
	}
}

// LoadTeams seeds the registry before the broker connection so the names are
// there when the first retained state arrives.
func (f *Fleet) LoadTeams(path string) int {
	if path == "" {
		return 0
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return 0
	}
	var tf teamsFile
	if json.Unmarshal(b, &tf) != nil {
		return 0
	}
	f.mu.Lock()
	for id, team := range tf.Teams {
		f.device(id).Team = team
	}
	f.mu.Unlock()
	return len(tf.Teams)
}

// RepublishTeams pushes every known team name as retained config, for a
// broker that lost its retained messages.
func (f *Fleet) RepublishTeams() {
	for _, d := range f.Snapshot() {
		if d.Team != "" {
			team := d.Team
			_ = f.publish(devTopic(d.ID, "cmd", "config"), Config{Team: &team, Locked: &d.Locked}, true)
		}
	}
}

// BridgeStatus describes the ESP-NOW bridge as last reported over MQTT.
func (f *Fleet) BridgeStatus() string {
	f.mu.RLock()
	defer f.mu.RUnlock()
	if f.bridgeText == "" {
		return "never seen"
	}
	return f.bridgeText
}

func NewFleet(bus *Bus) *Fleet {
	return &Fleet{devices: map[string]*Device{}, waiters: map[string][]chan ButtonEvent{}, bus: bus}
}

// Connect opens the broker session and subscribes to the whole tree. Retained
// state and config messages replay on subscribe, which is how the registry
// recovers after a restart.
func (f *Fleet) Connect(brokerURL string) error {
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID("showcase-server").
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(2 * time.Second).
		SetOrderMatters(false)
	opts.OnConnect = func(c mqtt.Client) {
		log.Printf("mqtt: connected to %s", brokerURL)
		if t := c.Subscribe(topicPrefix+"#", 1, f.onMessage); t.Wait() && t.Error() != nil {
			log.Printf("mqtt: subscribe failed: %v", t.Error())
		}
		f.RepublishTeams()
	}
	opts.OnConnectionLost = func(_ mqtt.Client, err error) {
		log.Printf("mqtt: connection lost: %v", err)
	}
	f.client = mqtt.NewClient(opts)
	t := f.client.Connect()
	t.Wait()
	return t.Error()
}

func (f *Fleet) Connected() bool {
	return f.client != nil && f.client.IsConnectionOpen()
}

func (f *Fleet) onMessage(_ mqtt.Client, m mqtt.Message) {
	parts := strings.Split(strings.TrimPrefix(m.Topic(), topicPrefix), "/")
	// dev/<id>/<group>/<name>
	if len(parts) == 4 && parts[0] == "dev" {
		id, group, name := parts[1], parts[2], parts[3]
		switch {
		case group == "state":
			// state has three parts; handled below
		case group == "event" && name == "button":
			f.onButton(id, m.Payload())
		case group == "cmd":
			f.onCommandSeen(id, name, m.Payload(), m.Retained())
		}
		return
	}
	if len(parts) == 3 && parts[0] == "dev" && parts[2] == "state" {
		f.onState(parts[1], m.Payload())
		return
	}
	if len(parts) == 2 && parts[0] == "bridge" && parts[1] == "state" {
		var st struct {
			Online bool   `json:"online"`
			Sent   int    `json:"sent"`
			Last   string `json:"last"`
		}
		if json.Unmarshal(m.Payload(), &st) == nil {
			text := "offline"
			if st.Online {
				text = fmt.Sprintf("online, %d frames sent, last %q", st.Sent, st.Last)
			}
			f.mu.Lock()
			changed := f.bridgeText != text
			f.bridgeText = text
			f.mu.Unlock()
			if changed {
				f.bus.Emit(Event{Kind: "presence", Device: "bridge", Text: "ESP-NOW bridge " + text})
			}
		}
		return
	}
	if len(parts) == 3 && parts[0] == "all" && parts[1] == "cmd" {
		f.mu.RLock()
		ids := make([]string, 0, len(f.devices))
		for id := range f.devices {
			ids = append(ids, id)
		}
		f.mu.RUnlock()
		for _, id := range ids {
			f.onCommandSeen(id, parts[2], m.Payload(), false)
		}
	}
}

func (f *Fleet) device(id string) *Device {
	d, ok := f.devices[id]
	if !ok {
		d = &Device{ID: id, Virtual: strings.HasPrefix(id, "fake")}
		f.devices[id] = d
	}
	return d
}

func (f *Fleet) onState(id string, payload []byte) {
	var s stateMsg
	if err := json.Unmarshal(payload, &s); err != nil {
		return
	}
	f.mu.Lock()
	d := f.device(id)
	wasOnline := d.Online
	d.Online = s.Online
	d.OTA = s.Online && s.OTA
	d.LastSeen = time.Now()
	if s.Online {
		d.Voice, d.Battery, d.NTP, d.Fired = s.Voice, s.Battery, s.NTP, s.Fired
		d.Code, d.Uptime, d.Heap, d.FW = s.Code, s.Uptime, s.Heap, s.FW
		d.IP, d.Chip, d.ImageMD5 = s.IP, s.Chip, s.ImageMD5
		if s.Team != "" {
			d.Team = s.Team
		}
	}
	team := d.Team
	f.mu.Unlock()
	if wasOnline != s.Online {
		f.bus.Emit(Event{Kind: "presence", Device: id, Team: team,
			Text: map[bool]string{true: "online", false: "offline"}[s.Online]})
	}
}

func (f *Fleet) onButton(id string, payload []byte) {
	var b ButtonEvent
	if err := json.Unmarshal(payload, &b); err != nil {
		return
	}
	b.Device, b.At = id, time.Now()
	f.mu.Lock()
	team := f.device(id).Team
	waiters := f.waiters[id]
	f.waiters[id] = nil
	f.mu.Unlock()
	for _, w := range waiters {
		select {
		case w <- b:
		default:
		}
	}
	f.bus.Emit(Event{Kind: "button", Device: id, Team: team, Text: b.Button + " " + b.Action})
}

// onCommandSeen keeps the dashboard model in step with every command on the
// broker, whoever sent it.
func (f *Fleet) onCommandSeen(id, name string, payload []byte, retained bool) {
	f.mu.Lock()
	defer f.mu.Unlock()
	d := f.device(id)
	switch name {
	case "display":
		var disp Display
		if json.Unmarshal(payload, &disp) == nil && disp.Text != "" {
			if disp.TTL <= 0 {
				disp.TTL = 15
			}
			disp.Until = time.Now().Add(time.Duration(disp.TTL) * time.Second)
			d.Screen = &disp
		}
	case "led":
		var led Led
		if json.Unmarshal(payload, &led) == nil {
			if led.TTL <= 0 {
				led.TTL = 15
			}
			led.Until = time.Now().Add(time.Duration(led.TTL) * time.Second)
			d.LED = &led
		}
	case "audio":
		var a Audio
		if json.Unmarshal(payload, &a) == nil {
			label := a.Jingle
			if label == "" {
				label = "notes: " + a.Notes
				if len(label) > 40 {
					label = label[:40] + "…"
				}
			}
			d.Audio = &AudioNote{Label: label, At: time.Now()}
		}
	case "config":
		var c Config
		if json.Unmarshal(payload, &c) == nil {
			if c.Team != nil {
				d.Team = *c.Team
			}
			if c.Locked != nil {
				d.Locked = *c.Locked
			}
		}
	}
}

// WaitButton blocks until the device reports a button press or the timeout.
func (f *Fleet) WaitButton(id string, timeout time.Duration, done <-chan struct{}) (ButtonEvent, bool) {
	ch := make(chan ButtonEvent, 1)
	f.mu.Lock()
	f.waiters[id] = append(f.waiters[id], ch)
	f.mu.Unlock()
	select {
	case ev := <-ch:
		return ev, true
	case <-time.After(timeout):
	case <-done:
	}
	f.mu.Lock()
	ws := f.waiters[id]
	for i, w := range ws {
		if w == ch {
			f.waiters[id] = append(ws[:i], ws[i+1:]...)
			break
		}
	}
	f.mu.Unlock()
	return ButtonEvent{}, false
}

// Snapshot returns a copy of every device, sorted by id.
func (f *Fleet) Snapshot() []Device {
	f.mu.RLock()
	defer f.mu.RUnlock()
	out := make([]Device, 0, len(f.devices))
	for _, d := range f.devices {
		out = append(out, *d)
	}
	sortDevices(out)
	return out
}

func sortDevices(ds []Device) {
	for i := 1; i < len(ds); i++ {
		for j := i; j > 0 && ds[j].ID < ds[j-1].ID; j-- {
			ds[j], ds[j-1] = ds[j-1], ds[j]
		}
	}
}

// ByCode finds the stick showing a claim code. Online devices win over stale
// registry entries with the same code.
func (f *Fleet) ByCode(code int) *Device {
	f.mu.RLock()
	defer f.mu.RUnlock()
	var best *Device
	for _, d := range f.devices {
		if d.Code != code || code == 0 {
			continue
		}
		if best == nil || (d.Online && !best.Online) {
			best = d
		}
	}
	if best == nil {
		return nil
	}
	c := *best
	return &c
}

// ByTeam finds a device by team name, case-insensitively.
func (f *Fleet) ByTeam(team string) *Device {
	f.mu.RLock()
	defer f.mu.RUnlock()
	for _, d := range f.devices {
		if d.Team != "" && strings.EqualFold(d.Team, team) {
			c := *d
			return &c
		}
	}
	return nil
}

func (f *Fleet) Get(id string) *Device {
	f.mu.RLock()
	defer f.mu.RUnlock()
	if d, ok := f.devices[id]; ok {
		c := *d
		return &c
	}
	return nil
}

// ---- publishing ----

func (f *Fleet) publish(topic string, v any, retain bool) error {
	payload, err := json.Marshal(v)
	if err != nil {
		return err
	}
	t := f.client.Publish(topic, 1, retain, payload)
	t.Wait()
	return t.Error()
}

// PublishRaw sends a plain-text payload, for the bridge protocol.
func (f *Fleet) PublishRaw(topic, payload string, retain bool) error {
	t := f.client.Publish(topic, 1, retain, []byte(payload))
	t.Wait()
	return t.Error()
}

func devTopic(id, group, name string) string {
	return fmt.Sprintf("%sdev/%s/%s/%s", topicPrefix, id, group, name)
}

func (f *Fleet) SendDisplay(id string, d Display) error {
	return f.publish(devTopic(id, "cmd", "display"), d, false)
}

func (f *Fleet) SendAudio(id string, a Audio) error {
	return f.publish(devTopic(id, "cmd", "audio"), a, false)
}

func (f *Fleet) SendLed(id string, l Led) error {
	return f.publish(devTopic(id, "cmd", "led"), l, false)
}

// SendConfig merges the change into the server's view and publishes the whole
// retained config. A retained MQTT message replaces the previous one, so a
// partial publish would silently drop the fields it left out.
func (f *Fleet) SendConfig(id string, c Config) error {
	f.mu.Lock()
	d := f.device(id)
	if c.Team != nil {
		d.Team = *c.Team
	}
	if c.Locked != nil {
		d.Locked = *c.Locked
	}
	team, locked := d.Team, d.Locked
	f.mu.Unlock()
	if c.Team != nil && f.onTeamsChanged != nil {
		f.onTeamsChanged()
	}
	return f.publish(devTopic(id, "cmd", "config"), Config{Team: &team, Locked: &locked}, true)
}

func (f *Fleet) BroadcastDisplay(d Display) error {
	return f.publish(topicPrefix+"all/cmd/display", d, false)
}

func (f *Fleet) BroadcastConfig(c Config) error {
	return f.publish(topicPrefix+"all/cmd/config", c, false)
}

func (f *Fleet) BroadcastAudio(a Audio) error {
	return f.publish(topicPrefix+"all/cmd/audio", a, false)
}

func (f *Fleet) BroadcastLed(l Led) error {
	return f.publish(topicPrefix+"all/cmd/led", l, false)
}
