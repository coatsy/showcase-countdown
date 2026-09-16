package main

import (
	"embed"
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"time"
)

//go:embed static/*
var staticFS embed.FS

type sendRequest struct {
	Code    string `json:"code,omitempty"`   // act as this team
	Secret  string `json:"secret,omitempty"` // or as the organiser
	Target  string `json:"target"`           // device id, "all", or a claim code
	Kind    string `json:"kind"`             // display, audio, led
	Text    string `json:"text,omitempty"`
	Seconds int    `json:"seconds,omitempty"`
	Jingle  string `json:"jingle,omitempty"`
	Notes   string `json:"notes,omitempty"`
	Color   string `json:"color,omitempty"`
	Mode    string `json:"mode,omitempty"`
}

func defaultOTARunner(targets []string) error {
	if len(targets) == 0 {
		return fmt.Errorf("no targets selected for OTA update")
	}
	cmdText := strings.TrimSpace(os.Getenv("OTA_RUNNER"))
	if cmdText == "" {
		return fmt.Errorf("OTA_RUNNER is not configured; set it to a command template like: OTA_RUNNER='python ../scripts/ota_push.py --target {target}'")
	}
	for _, target := range targets {
		cmd := strings.ReplaceAll(cmdText, "{target}", target)
		cmd = strings.ReplaceAll(cmd, "{targets}", strings.Join(targets, ","))
		parts, err := shellQuote(cmd)
		if err != nil {
			return fmt.Errorf("invalid OTA_RUNNER template: %w", err)
		}
		if err := exec.Command(parts[0], parts[1:]...).Run(); err != nil {
			return fmt.Errorf("OTA update failed for %s: %w", target, err)
		}
	}
	return nil
}

func shellQuote(text string) ([]string, error) {
	if text == "" {
		return nil, fmt.Errorf("empty command")
	}
	var shell, flag string
	if runtime.GOOS == "windows" {
		shell = "powershell"
		flag = "-Command"
	} else {
		shell = "/bin/sh"
		flag = "-lc"
	}
	cmd := exec.Command(shell, flag, text)
	if cmd.Args == nil {
		return nil, fmt.Errorf("failed to build command")
	}
	return append([]string{cmd.Path}, cmd.Args[1:]...), nil
}

func (a *App) dashboardRoutes(mux *http.ServeMux) {
	sub, _ := fs.Sub(staticFS, "static")
	mux.Handle("/", http.FileServer(http.FS(sub)))

	if a.otaRunner == nil {
		a.otaRunner = defaultOTARunner
	}

	mux.HandleFunc("GET /api/fleet", func(w http.ResponseWriter, r *http.Request) {
		manual, auto := a.policy.LockState()
		writeJSON(w, map[string]any{
			"title": a.title, "event_epoch": a.epoch, "now": time.Now(),
			"manual_lock": manual, "auto_lock": auto, "muted": a.policy.MutedIDs(),
			"broker": a.fleet.Connected(), "devices": a.fleet.Snapshot(),
		})
	})

	mux.HandleFunc("GET /api/events", func(w http.ResponseWriter, r *http.Request) {
		flusher, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "streaming unsupported", 500)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-cache")
		lastID, _ := strconv.ParseUint(r.Header.Get("Last-Event-ID"), 10, 64)
		for _, e := range a.bus.RecentAfter(lastID, 100) {
			writeSSE(w, e)
		}
		flusher.Flush()
		ch, cancel := a.bus.Subscribe()
		defer cancel()
		keep := time.NewTicker(20 * time.Second)
		defer keep.Stop()
		for {
			select {
			case e := <-ch:
				writeSSE(w, e)
				flusher.Flush()
			case <-keep.C:
				fmt.Fprint(w, ": keepalive\n\n")
				flusher.Flush()
			case <-r.Context().Done():
				return
			}
		}
	})

	// Send from the browser, as a team (claim code) or as the organiser.
	mux.HandleFunc("POST /api/send", func(w http.ResponseWriter, r *http.Request) {
		var req sendRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "bad json", 400)
			return
		}
		organiser := a.secret != "" && req.Secret == a.secret
		var from, fromID string
		if !organiser {
			code, err := strconv.Atoi(strings.TrimSpace(req.Code))
			if err != nil {
				http.Error(w, "need a claim code or the organiser secret", 401)
				return
			}
			d := a.fleet.ByCode(code)
			if d == nil {
				http.Error(w, "no stick shows that code", 404)
				return
			}
			if err := a.policy.Gate(d.ID); err != nil {
				http.Error(w, err.Error(), 423)
				return
			}
			from, fromID = teamLabel(d), d.ID
			if req.Target == "" || req.Target == "self" {
				req.Target = d.ID
			}
		} else {
			from = "Organiser"
		}
		targets, err := a.resolveTargets(req.Target)
		if err != nil {
			http.Error(w, err.Error(), 404)
			return
		}
		if !organiser && (req.Target == "all" || len(targets) != 1 || targets[0] != fromID) {
			http.Error(w, "teams may only send to their own stick from here; use the MCP tools for shouts and messages", 403)
			return
		}
		if err := a.sendKind(req, targets, from, organiser); err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		writeJSON(w, map[string]any{"ok": true, "targets": targets})
	})

	mux.HandleFunc("POST /api/organiser/{action}", func(w http.ResponseWriter, r *http.Request) {
		if a.secret == "" || r.Header.Get(headerSecret) != a.secret {
			http.Error(w, "organiser secret required", 401)
			return
		}
		team := r.URL.Query().Get("team")
		switch r.PathValue("action") {
		case "lock":
			a.setLock(true)
		case "unlock":
			a.setLock(false)
		case "mute", "unmute":
			d := a.findTeamOrCode(team)
			if d == nil {
				http.Error(w, "no such team", 404)
				return
			}
			a.policy.SetMuted(d.ID, r.PathValue("action") == "mute")
			a.bus.Emit(Event{Kind: "organiser", Device: d.ID, Team: teamLabel(d), Text: r.PathValue("action") + "d"})
		case "ota":
			target := strings.TrimSpace(r.URL.Query().Get("target"))
			if target == "" {
				target = "all"
			}
			targets, err := a.resolveOTATargets(target)
			if err != nil {
				http.Error(w, err.Error(), 404)
				return
			}
			if err := a.otaRunner(targets); err != nil {
				http.Error(w, err.Error(), 500)
				return
			}
			writeJSON(w, map[string]any{"ok": true, "targets": targets})
			return
		default:
			http.Error(w, "unknown action", 404)
			return
		}
		writeJSON(w, map[string]any{"ok": true})
	})

	// Buttons on virtual sticks.
	mux.HandleFunc("POST /api/fake/{id}/button", func(w http.ResponseWriter, r *http.Request) {
		f := fakeByID(r.PathValue("id"))
		if f == nil {
			http.Error(w, "no such fake", 404)
			return
		}
		button := strings.ToUpper(r.URL.Query().Get("b"))
		action := r.URL.Query().Get("action")
		if button == "" {
			button = "A"
		}
		if action == "" {
			action = "click"
		}
		f.pressButton(button, action)
		writeJSON(w, map[string]any{"ok": true})
	})
}

func (a *App) resolveTargets(target string) ([]string, error) {
	target = strings.TrimSpace(target)
	if target == "all" {
		var ids []string
		for _, d := range a.fleet.Snapshot() {
			ids = append(ids, d.ID)
		}
		return ids, nil
	}
	if d := a.fleet.Get(target); d != nil {
		return []string{d.ID}, nil
	}
	if d := a.findTeamOrCode(target); d != nil {
		return []string{d.ID}, nil
	}
	return nil, fmt.Errorf("no device, team or code %q", target)
}

func (a *App) resolveOTATargets(target string) ([]string, error) {
	target = strings.TrimSpace(target)
	if target == "all" {
		var ips []string
		for _, d := range a.fleet.Snapshot() {
			if !d.Online || !d.OTA {
				continue
			}
			if d.IP != "" {
				ips = append(ips, d.IP)
				continue
			}
			ips = append(ips, d.ID)
		}
		if len(ips) == 0 {
			return nil, fmt.Errorf("no online devices available for OTA update")
		}
		return ips, nil
	}
	if d := a.fleet.Get(target); d != nil {
		if d.IP != "" {
			return []string{d.IP}, nil
		}
		return []string{d.ID}, nil
	}
	if d := a.findTeamOrCode(target); d != nil {
		if d.IP != "" {
			return []string{d.IP}, nil
		}
		return []string{d.ID}, nil
	}
	return nil, fmt.Errorf("no device, team or code %q", target)
}

func (a *App) sendKind(req sendRequest, targets []string, from string, organiser bool) error {
	kind := "self"
	priority := 0
	if organiser {
		kind, priority = "organiser", 255
	}
	for _, id := range targets {
		switch req.Kind {
		case "display":
			msg, err := cleanText(req.Text)
			if err != nil {
				return err
			}
			if err := a.fleet.SendDisplay(id, Display{Text: msg, From: from, Kind: kind, TTL: clampTTL(req.Seconds, 15), Priority: priority}); err != nil {
				return err
			}
			a.bus.Emit(Event{Kind: "display", Device: id, Team: from, Text: msg})
		case "audio":
			if req.Jingle == "" && req.Notes == "" {
				return fmt.Errorf("jingle or notes required")
			}
			if err := a.fleet.SendAudio(id, Audio{Jingle: req.Jingle, Notes: req.Notes}); err != nil {
				return err
			}
			a.bus.Emit(Event{Kind: "audio", Device: id, Team: from, Text: req.Jingle + req.Notes})
		case "led":
			mode := req.Mode
			if mode == "" {
				mode = "solid"
			}
			if err := a.fleet.SendLed(id, Led{Color: req.Color, Mode: mode, TTL: clampTTL(req.Seconds, 15)}); err != nil {
				return err
			}
			a.bus.Emit(Event{Kind: "led", Device: id, Team: from, Text: req.Color + " " + mode})
		default:
			return fmt.Errorf("kind must be display, audio or led")
		}
	}
	return nil
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func writeSSE(w http.ResponseWriter, e Event) {
	b, _ := json.Marshal(e)
	fmt.Fprintf(w, "id: %d\ndata: %s\n\n", e.id, b)
}
