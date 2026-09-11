package main

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

const (
	headerCode   = "X-Claim-Code"
	headerSecret = "X-Organiser-Secret"
)

// App wires the fleet, policy, and bus behind the MCP tools and the dashboard.
type App struct {
	fleet  *Fleet
	policy *Policy
	bus    *Bus
	bridge *SerialBridge // optional USB path; MQTT is the usual one
	secret string
	title  string
	epoch  time.Time
}

func text(format string, args ...any) *mcp.CallToolResult {
	return &mcp.CallToolResult{Content: []mcp.Content{&mcp.TextContent{Text: fmt.Sprintf(format, args...)}}}
}

func jsonResult(v any) *mcp.CallToolResult {
	b, _ := json.MarshalIndent(v, "", "  ")
	return &mcp.CallToolResult{Content: []mcp.Content{&mcp.TextContent{Text: string(b)}}}
}

func headerValue(req *mcp.CallToolRequest, name string) string {
	if req == nil || req.Extra == nil || req.Extra.Header == nil {
		return ""
	}
	return strings.TrimSpace(req.Extra.Header.Get(name))
}

// teamDevice resolves the caller's claim code to a stick.
func (a *App) teamDevice(req *mcp.CallToolRequest) (*Device, error) {
	raw := headerValue(req, headerCode)
	if raw == "" {
		return nil, fmt.Errorf("no claim code: connect to /mcp/<code> using the four-digit code shown on your stick, or send the %s header", headerCode)
	}
	code, err := strconv.Atoi(raw)
	if err != nil {
		return nil, fmt.Errorf("claim code %q is not a number", raw)
	}
	d := a.fleet.ByCode(code)
	if d == nil {
		return nil, fmt.Errorf("no stick is showing code %04d; check the number in the bottom-left of the screen", code)
	}
	return d, nil
}

func (a *App) organiser(req *mcp.CallToolRequest) error {
	if a.secret == "" {
		return fmt.Errorf("organiser tools are disabled: no organiser secret configured")
	}
	if headerValue(req, headerSecret) != a.secret {
		return fmt.Errorf("organiser secret missing or wrong (%s header)", headerSecret)
	}
	return nil
}

func teamLabel(d *Device) string {
	if d.Team != "" {
		return d.Team
	}
	return fmt.Sprintf("Table %04d", d.Code)
}

func normalizeColor(raw string) (string, error) {
	value := strings.TrimSpace(raw)
	if value == "" {
		return "", fmt.Errorf("color is empty")
	}
	if strings.HasPrefix(strings.ToLower(value), "#") {
		value = value[1:]
	}
	if len(value) == 6 {
		if _, err := strconv.ParseUint(value, 16, 32); err == nil {
			return "#" + strings.ToUpper(value), nil
		}
	}

	named := map[string]string{
		"black":   "#000000",
		"off":     "#000000",
		"red":     "#FF0000",
		"green":   "#00FF00",
		"blue":    "#0000FF",
		"yellow":  "#FFFF00",
		"cyan":    "#00FFFF",
		"magenta": "#FF00FF",
		"white":   "#FFFFFF",
		"orange":  "#FFA500",
		"grey":    "#808080",
		"gray":    "#808080",
		"skyblue": "#87CEEB",
		"pink":    "#FFC0CB",
	}
	if hex, ok := named[strings.ToLower(value)]; ok {
		return hex, nil
	}
	return "", fmt.Errorf("unsupported color %q; use #RRGGBB or a named colour like red, blue, skyblue, orange, pink, white, or off", raw)
}

// ---- tool inputs ----

type claimIn struct {
	TeamName string `json:"team_name" jsonschema:"the name your team wants shown on the stick and to other teams"`
}
type showIn struct {
	Text    string `json:"text" jsonschema:"message to show, up to 120 characters; colour markup like [red]hot[/] or [#00A4EF]blue[/] is allowed"`
	Seconds int    `json:"seconds,omitempty" jsonschema:"how long to show it, 1-60 seconds (default 15)"`
}
type playIn struct {
	Jingle string `json:"jingle,omitempty" jsonschema:"name of a built-in jingle; call jingles to list them"`
	Notes  string `json:"notes,omitempty" jsonschema:"composed tune as NOTE:MS tokens, e.g. 'C5:200 E5:200 G5:200 C6:400 R:100 G5:150'; R is a rest, sharps and flats like F#4 or Bb4 work"`
	Volume int    `json:"volume,omitempty" jsonschema:"0-255, default leaves the current volume"`
}
type ledIn struct {
	Color    string `json:"color" jsonschema:"hex colour like #FF6600 or a named colour like red, blue, skyblue, orange or off; ignored by rainbow and rolling_rainbow"`
	Mode     string `json:"mode,omitempty" jsonschema:"solid, blink, breathe, off, snake, ping, rainbow or rolling_rainbow (default solid)"`
	PeriodMs int    `json:"period_ms,omitempty" jsonschema:"animation period in milliseconds: blink/breathe cycle, snake lap, ping round trip or rainbow rotation (default 600)"`
	Seconds  int    `json:"seconds,omitempty" jsonschema:"how long before the pixel returns to its schedule, 1-60 (default 15)"`
}
type shoutIn struct {
	Text string `json:"text" jsonschema:"what to shout to every stick in the room, up to 120 characters"`
}
type messageIn struct {
	Team string `json:"team" jsonschema:"the other team's name (see list_teams)"`
	Text string `json:"text" jsonschema:"the message, up to 120 characters"`
}
type waitIn struct {
	TimeoutSeconds int `json:"timeout_seconds,omitempty" jsonschema:"how long to wait for a button press, 1-120 (default 30)"`
}
type broadcastIn struct {
	Text    string `json:"text" jsonschema:"message for every stick"`
	Seconds int    `json:"seconds,omitempty" jsonschema:"1-60 (default 20)"`
}
type broadcastAudioIn struct {
	Jingle string `json:"jingle,omitempty" jsonschema:"a built-in jingle to play on every stick; call jingles to list them"`
	Notes  string `json:"notes,omitempty" jsonschema:"a composed tune to play on every stick, NOTE:MS tokens like 'G5:180 C6:180 C7:500'"`
	Volume int    `json:"volume,omitempty" jsonschema:"0-255, default leaves each stick's current volume"`
}
type teamIn struct {
	Team string `json:"team" jsonschema:"team name or four-digit claim code"`
}
type renameIn struct {
	Team    string `json:"team" jsonschema:"current team name or four-digit claim code"`
	NewName string `json:"new_name" jsonschema:"the new team name"`
}
type shoutCooldownIn struct {
	Seconds int `json:"seconds" jsonschema:"per-team shout gap in seconds; 0 removes the limit, higher calms a noisy room (max 3600)"`
}

// findTeamOrCode accepts a team name, a bare claim code, or the default
// "Table 1234" label an unnamed stick shows in list_teams.
func (a *App) findTeamOrCode(s string) *Device {
	s = strings.TrimSpace(s)
	if code, err := strconv.Atoi(s); err == nil {
		return a.fleet.ByCode(code)
	}
	if rest, ok := strings.CutPrefix(strings.ToLower(s), "table"); ok {
		if code, err := strconv.Atoi(strings.TrimSpace(rest)); err == nil {
			return a.fleet.ByCode(code)
		}
	}
	return a.fleet.ByTeam(s)
}

// mcpServer registers every tool. One server instance serves all sessions;
// identity is per call, from the headers.
func (a *App) mcpServer() *mcp.Server {
	s := mcp.NewServer(&mcp.Implementation{Name: "showcase-sticks", Version: "0.3.0"}, nil)

	mcp.AddTool(s, &mcp.Tool{Name: "claim", Description: "Name your team. The claim code on your stick's screen identifies it; this sets the name shown on the stick and to other teams."},
		func(ctx context.Context, req *mcp.CallToolRequest, in claimIn) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			name, err := cleanText(in.TeamName)
			if err != nil {
				return nil, nil, err
			}
			if len([]rune(name)) > 24 {
				return nil, nil, fmt.Errorf("team name is too long: keep it under 24 characters")
			}
			if other := a.fleet.ByTeam(name); other != nil && other.ID != d.ID {
				return nil, nil, fmt.Errorf("another stick already uses the name %q", name)
			}
			if err := a.fleet.SendConfig(d.ID, Config{Team: &name}); err != nil {
				return nil, nil, err
			}
			a.bus.Emit(Event{Kind: "claim", Device: d.ID, Team: name, Text: fmt.Sprintf("code %04d is now %s", d.Code, name)})
			return text("Stick %s (code %04d) is now %q.", d.ID, d.Code, name), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "status", Description: "Your stick's current state: online, team, battery, time sync, what it is showing, and whether the room is locked."},
		func(ctx context.Context, req *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			manual, auto := a.policy.LockState()
			out := map[string]any{
				"device": d, "team": teamLabel(d), "room_locked": manual || auto,
				"muted": a.policy.Muted(d.ID), "countdown_target": a.epoch,
				"seconds_remaining": int(time.Until(a.epoch).Seconds()),
			}
			return jsonResult(out), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "show", Description: "Show a message on your own stick for a few seconds. Colour markup is allowed."},
		func(ctx context.Context, req *mcp.CallToolRequest, in showIn) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			if err := a.policy.Gate(d.ID); err != nil {
				return nil, nil, err
			}
			msg, err := cleanText(in.Text)
			if err != nil {
				return nil, nil, err
			}
			ttl := clampTTL(in.Seconds, 15)
			if err := a.fleet.SendDisplay(d.ID, Display{Text: msg, From: teamLabel(d), Kind: "self", TTL: ttl}); err != nil {
				return nil, nil, err
			}
			a.bus.Emit(Event{Kind: "display", Device: d.ID, Team: teamLabel(d), Text: msg})
			return text("Showing on %s for %d s.", teamLabel(d), ttl), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "jingles", Description: "List the built-in jingles you can play."},
		func(ctx context.Context, req *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			return jsonResult(jingleNames), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "play", Description: "Play audio on your own stick: a named jingle, or a tune you compose from NOTE:MS tokens."},
		func(ctx context.Context, req *mcp.CallToolRequest, in playIn) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			if err := a.policy.Gate(d.ID); err != nil {
				return nil, nil, err
			}
			in.Jingle = strings.TrimSpace(strings.ToLower(in.Jingle))
			in.Notes = strings.TrimSpace(in.Notes)
			if in.Jingle == "" && in.Notes == "" {
				return nil, nil, fmt.Errorf("give a jingle name or a notes string")
			}
			if in.Jingle != "" && !knownJingle(in.Jingle) {
				return nil, nil, fmt.Errorf("unknown jingle %q; call jingles to see the list", in.Jingle)
			}
			if len(in.Notes) > 380 {
				return nil, nil, fmt.Errorf("notes string is too long for one command (380 characters); send it in parts")
			}
			if err := a.fleet.SendAudio(d.ID, Audio{Jingle: in.Jingle, Notes: in.Notes, Volume: in.Volume}); err != nil {
				return nil, nil, err
			}
			label := in.Jingle
			if label == "" {
				label = "composed tune"
			}
			a.bus.Emit(Event{Kind: "audio", Device: d.ID, Team: teamLabel(d), Text: label})
			return text("Playing %s on %s.", label, teamLabel(d)), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "led", Description: "Set the colour and pattern of your stick's LED for a while."},
		func(ctx context.Context, req *mcp.CallToolRequest, in ledIn) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			if err := a.policy.Gate(d.ID); err != nil {
				return nil, nil, err
			}
			mode := strings.ToLower(strings.TrimSpace(in.Mode))
			if mode == "" {
				mode = "solid"
			}
			// Callers lower-case "RollingRainbow" without adding a separator.
			if mode == "rollingrainbow" {
				mode = "rolling_rainbow"
			}
			switch mode {
			case "solid", "blink", "breathe", "off", "snake", "ping", "rainbow", "rolling_rainbow":
			default:
				return nil, nil, fmt.Errorf("mode must be solid, blink, breathe, off, snake, ping, rainbow or rolling_rainbow")
			}
			color, err := normalizeColor(in.Color)
			if err != nil {
				return nil, nil, err
			}
			ttl := clampTTL(in.Seconds, 15)
			if err := a.fleet.SendLed(d.ID, Led{Color: color, Mode: mode, PeriodMs: in.PeriodMs, TTL: ttl}); err != nil {
				return nil, nil, err
			}
			a.bus.Emit(Event{Kind: "led", Device: d.ID, Team: teamLabel(d), Text: color + " " + mode})
			return text("LED on %s set to %s %s for %d s.", teamLabel(d), color, mode, ttl), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "shout", Description: "Shout to every stick in the room, with your team's name, an alarm at volume 200/255 and 15 seconds of flashing amber LEDs. Cooldown applies."},
		func(ctx context.Context, req *mcp.CallToolRequest, in shoutIn) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			if err := a.policy.Gate(d.ID); err != nil {
				return nil, nil, err
			}
			msg, err := cleanText(in.Text)
			if err != nil {
				return nil, nil, err
			}
			if err := a.policy.CheckShout(d.ID); err != nil {
				return nil, nil, err
			}
			from := teamLabel(d)
			if err := a.fleet.BroadcastDisplay(Display{Text: msg, From: from, Kind: "shout", TTL: 15, Priority: 1}); err != nil {
				return nil, nil, err
			}
			if err := a.fleet.BroadcastAudio(Audio{Jingle: "alarm", Volume: 200}); err != nil {
				return nil, nil, fmt.Errorf("shout text sent, but notification sound failed: %w", err)
			}
			if err := a.fleet.BroadcastLed(Led{Color: "#FFAA00", Mode: "blink", PeriodMs: 600, TTL: 15}); err != nil {
				return nil, nil, fmt.Errorf("shout text and sound sent, but LED flash failed: %w", err)
			}
			for _, other := range a.fleet.Snapshot() {
				if other.ID != d.ID {
					a.policy.Deliver(other.ID, Message{From: from, Kind: "shout", Text: msg})
				}
			}
			a.bus.Emit(Event{Kind: "shout", Device: d.ID, Team: from, Text: msg})
			return text("%s shouted to the room.", from), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "message_team", Description: "Send a message to another team's stick and inbox."},
		func(ctx context.Context, req *mcp.CallToolRequest, in messageIn) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			if err := a.policy.Gate(d.ID); err != nil {
				return nil, nil, err
			}
			msg, err := cleanText(in.Text)
			if err != nil {
				return nil, nil, err
			}
			target := a.findTeamOrCode(in.Team)
			if target == nil {
				return nil, nil, fmt.Errorf("no team called %q; call list_teams", in.Team)
			}
			if target.ID == d.ID {
				return nil, nil, fmt.Errorf("that is your own stick; use show instead")
			}
			if err := a.policy.CheckDM(d.ID); err != nil {
				return nil, nil, err
			}
			from := teamLabel(d)
			if err := a.fleet.SendDisplay(target.ID, Display{Text: msg, From: from, Kind: "dm", TTL: 15}); err != nil {
				return nil, nil, err
			}
			a.policy.Deliver(target.ID, Message{From: from, Kind: "dm", Text: msg})
			a.bus.Emit(Event{Kind: "dm", Device: target.ID, Team: from, Text: fmt.Sprintf("to %s: %s", teamLabel(target), msg)})
			return text("Sent to %s.", teamLabel(target)), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "list_teams", Description: "Every stick in the room: team name, whether it is online, and what voice it plays in the fanfare."},
		func(ctx context.Context, req *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			type row struct {
				Team   string `json:"team"`
				Online bool   `json:"online"`
				Voice  string `json:"voice,omitempty"`
			}
			var rows []row
			for _, d := range a.fleet.Snapshot() {
				rows = append(rows, row{Team: teamLabel(&d), Online: d.Online, Voice: d.Voice})
			}
			return jsonResult(rows), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "inbox", Description: "Messages other teams have sent you since you last checked, and shouts from the room."},
		func(ctx context.Context, req *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			box := a.policy.Inbox(d.ID)
			if len(box) == 0 {
				return text("Inbox empty."), nil, nil
			}
			return jsonResult(box), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "wait_for_button", Description: "Wait until someone presses a button on your stick. Returns which button (A or B) and how (click, double, press)."},
		func(ctx context.Context, req *mcp.CallToolRequest, in waitIn) (*mcp.CallToolResult, any, error) {
			d, err := a.teamDevice(req)
			if err != nil {
				return nil, nil, err
			}
			timeout := in.TimeoutSeconds
			if timeout <= 0 {
				timeout = 30
			}
			if timeout > 120 {
				timeout = 120
			}
			ev, ok := a.fleet.WaitButton(d.ID, time.Duration(timeout)*time.Second, ctx.Done())
			if !ok {
				return text("No button press within %d s.", timeout), nil, nil
			}
			return jsonResult(ev), nil, nil
		})

	// ---- organiser ----

	mcp.AddTool(s, &mcp.Tool{Name: "broadcast", Description: "Organiser: show a message on every stick. Overrides team messages."},
		func(ctx context.Context, req *mcp.CallToolRequest, in broadcastIn) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			msg, err := cleanText(in.Text)
			if err != nil {
				return nil, nil, err
			}
			ttl := clampTTL(in.Seconds, 20)
			if err := a.fleet.BroadcastDisplay(Display{Text: msg, From: "Organiser", Kind: "organiser", TTL: ttl, Priority: 255}); err != nil {
				return nil, nil, err
			}
			a.bus.Emit(Event{Kind: "organiser", Text: msg})
			return text("Broadcast for %d s.", ttl), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "broadcast_audio", Description: "Organiser: play a jingle or a composed tune on every stick in the room at once."},
		func(ctx context.Context, req *mcp.CallToolRequest, in broadcastAudioIn) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			in.Jingle = strings.TrimSpace(strings.ToLower(in.Jingle))
			in.Notes = strings.TrimSpace(in.Notes)
			if in.Jingle == "" && in.Notes == "" {
				return nil, nil, fmt.Errorf("give a jingle name or a notes string")
			}
			if in.Jingle != "" && !knownJingle(in.Jingle) {
				return nil, nil, fmt.Errorf("unknown jingle %q; call jingles to see the list", in.Jingle)
			}
			if len(in.Notes) > 380 {
				return nil, nil, fmt.Errorf("notes string is too long for one command (380 characters)")
			}
			if err := a.fleet.BroadcastAudio(Audio{Jingle: in.Jingle, Notes: in.Notes, Volume: in.Volume}); err != nil {
				return nil, nil, err
			}
			label := in.Jingle
			if label == "" {
				label = "composed tune"
			}
			a.bus.Emit(Event{Kind: "organiser", Text: "room audio: " + label})
			return text("Playing %s on every stick.", label), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "lock", Description: "Organiser: lock the room. Team commands are refused and sticks ignore them until unlock."},
		func(ctx context.Context, req *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			a.setLock(true)
			return text("Room locked."), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "unlock", Description: "Organiser: unlock the room."},
		func(ctx context.Context, req *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			a.setLock(false)
			return text("Room unlocked."), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "shout_cooldown", Description: "Organiser: change the per-team shout gap live. 0 removes it; raise it if the room gets noisy."},
		func(ctx context.Context, req *mcp.CallToolRequest, in shoutCooldownIn) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			if in.Seconds < 0 || in.Seconds > 3600 {
				return nil, nil, fmt.Errorf("seconds must be between 0 and 3600")
			}
			a.policy.SetShoutCooldown(time.Duration(in.Seconds) * time.Second)
			a.bus.Emit(Event{Kind: "organiser", Text: fmt.Sprintf("shout cooldown set to %d s", in.Seconds)})
			if in.Seconds == 0 {
				return text("Shout cooldown removed."), nil, nil
			}
			return text("Shout cooldown is now %d seconds per team.", in.Seconds), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "mute", Description: "Organiser: stop a team's stick accepting commands."},
		func(ctx context.Context, req *mcp.CallToolRequest, in teamIn) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			d := a.findTeamOrCode(in.Team)
			if d == nil {
				return nil, nil, fmt.Errorf("no team or code %q", in.Team)
			}
			a.policy.SetMuted(d.ID, true)
			a.bus.Emit(Event{Kind: "organiser", Device: d.ID, Team: teamLabel(d), Text: "muted"})
			return text("%s muted.", teamLabel(d)), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "unmute", Description: "Organiser: let a muted team's stick accept commands again."},
		func(ctx context.Context, req *mcp.CallToolRequest, in teamIn) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			d := a.findTeamOrCode(in.Team)
			if d == nil {
				return nil, nil, fmt.Errorf("no team or code %q", in.Team)
			}
			a.policy.SetMuted(d.ID, false)
			a.bus.Emit(Event{Kind: "organiser", Device: d.ID, Team: teamLabel(d), Text: "unmuted"})
			return text("%s unmuted.", teamLabel(d)), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "rename", Description: "Organiser: rename a team."},
		func(ctx context.Context, req *mcp.CallToolRequest, in renameIn) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			d := a.findTeamOrCode(in.Team)
			if d == nil {
				return nil, nil, fmt.Errorf("no team or code %q", in.Team)
			}
			name, err := cleanText(in.NewName)
			if err != nil {
				return nil, nil, err
			}
			if err := a.fleet.SendConfig(d.ID, Config{Team: &name}); err != nil {
				return nil, nil, err
			}
			a.bus.Emit(Event{Kind: "claim", Device: d.ID, Team: name, Text: "renamed by organiser"})
			return text("Renamed to %q.", name), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "unbind", Description: "Organiser: clear a stick's team name."},
		func(ctx context.Context, req *mcp.CallToolRequest, in teamIn) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			d := a.findTeamOrCode(in.Team)
			if d == nil {
				return nil, nil, fmt.Errorf("no team or code %q", in.Team)
			}
			empty := ""
			if err := a.fleet.SendConfig(d.ID, Config{Team: &empty}); err != nil {
				return nil, nil, err
			}
			a.bus.Emit(Event{Kind: "claim", Device: d.ID, Text: "unbound"})
			return text("Stick %s unbound.", d.ID), nil, nil
		})

	mcp.AddTool(s, &mcp.Tool{Name: "room", Description: "Organiser: lock state, muted teams, and the whole fleet."},
		func(ctx context.Context, req *mcp.CallToolRequest, _ struct{}) (*mcp.CallToolResult, any, error) {
			if err := a.organiser(req); err != nil {
				return nil, nil, err
			}
			manual, auto := a.policy.LockState()
			return jsonResult(map[string]any{
				"manual_lock": manual, "auto_lock": auto, "muted": a.policy.MutedIDs(),
				"shout_cooldown_s": int(a.policy.ShoutCooldown().Seconds()),
				"bridge":           a.fleet.BridgeStatus(), "devices": a.fleet.Snapshot(),
			}), nil, nil
		})

	return s
}

// setLock flips the manual lock and tells every stick, over MQTT and, when
// fitted, over the ESP-NOW bridge.
func (a *App) setLock(on bool) {
	a.policy.SetManualLock(on)
	locked := on
	_ = a.fleet.BroadcastConfig(Config{Locked: &locked})
	for _, d := range a.fleet.Snapshot() {
		_ = a.fleet.SendConfig(d.ID, Config{Locked: &locked})
	}
	if on {
		a.bridgeSend("lock")
	} else {
		a.bridgeSend("unlock")
	}
	a.bus.Emit(Event{Kind: "lock", Text: map[bool]string{true: "room locked", false: "room unlocked"}[on]})
}

var jingleNames = []string{"ding", "tada", "merge", "coin", "levelup", "alarm", "sad", "fail", "knock", "ok"}

func knownJingle(name string) bool {
	for _, j := range jingleNames {
		if j == name {
			return true
		}
	}
	return false
}
