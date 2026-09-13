package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestDashboardDeviceTeams(test *testing.T) {
	bus := NewBus(10)
	fleet := NewFleet(bus)
	app := &App{fleet: fleet, policy: NewPolicy(time.Time{}), bus: bus}
	mux := http.NewServeMux()
	app.dashboardRoutes(mux)
	for _, team := range []string{
		"Headwaters", "Atlas", "Outpost", "Gateway", "Trailblazer", "Sentinel",
		"Horizon", "Basecamp", "Wayfinder", "Relay", "Waypoint",
	} {
		test.Run(team, func(test *testing.T) {
			payload, err := json.Marshal(stateMsg{Online: true, Code: 1234, Team: team})
			if err != nil {
				test.Fatal(err)
			}
			fleet.onState("device-1", payload)
			response := httptest.NewRecorder()
			mux.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/api/fleet", nil))
			if response.Code != http.StatusOK {
				test.Fatalf("fleet returned HTTP %d: %s", response.Code, response.Body.String())
			}
			var snapshot struct {
				Devices []Device `json:"devices"`
			}
			if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil {
				test.Fatal(err)
			}
			if len(snapshot.Devices) != 1 || snapshot.Devices[0].Team != team {
				test.Fatalf("team update did not reach dashboard: %+v", snapshot.Devices)
			}
		})
	}
}

func TestMCPProtocolMetadata(t *testing.T) {
	for _, version := range []string{"2025-03-26", "2025-11-25"} {
		for _, withMeta := range []bool{false, true} {
			name := version
			if withMeta {
				name += "/metadata"
			}
			t.Run(name, func(t *testing.T) {
				app := &App{fleet: NewFleet(NewBus(10)), policy: NewPolicy(time.Time{}), secret: "test-only"}
				server := httptest.NewServer(app.mcpHandler())
				defer server.Close()
				sessionID := ""
				rpc := func(method string, params map[string]any, secret string) map[string]json.RawMessage {
					t.Helper()
					if withMeta && method != "initialize" {
						params["_meta"] = map[string]string{"io.modelcontextprotocol/protocolVersion": version}
					}
					body, err := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
					if err != nil {
						t.Fatal(err)
					}
					req, err := http.NewRequest(http.MethodPost, server.URL, bytes.NewReader(body))
					if err != nil {
						t.Fatal(err)
					}
					req.Header.Set("Content-Type", "application/json")
					req.Header.Set("Accept", "application/json, text/event-stream")
					req.Header.Set("Mcp-Protocol-Version", version)
					req.Header.Set("Mcp-Session-Id", sessionID)
					req.Header.Set(headerSecret, secret)
					resp, err := server.Client().Do(req)
					if err != nil {
						t.Fatal(err)
					}
					defer resp.Body.Close()
					raw, err := io.ReadAll(resp.Body)
					if err != nil {
						t.Fatal(err)
					}
					if resp.StatusCode != http.StatusOK {
						t.Fatalf("%s: HTTP %d: %s", method, resp.StatusCode, raw)
					}
					if id := resp.Header.Get("Mcp-Session-Id"); id != "" {
						sessionID = id
					}
					for _, line := range strings.Split(string(raw), "\n") {
						if data, ok := strings.CutPrefix(line, "data:"); ok {
							raw = []byte(strings.TrimSpace(data))
							break
						}
					}
					var result map[string]json.RawMessage
					if err := json.Unmarshal(raw, &result); err != nil {
						t.Fatalf("%s: %v: %s", method, err, raw)
					}
					if e := result["error"]; e != nil {
						t.Fatalf("%s: %s", method, e)
					}
					return result
				}
				rpc("initialize", map[string]any{
					"protocolVersion": version, "capabilities": map[string]any{},
					"clientInfo": map[string]string{"name": "compatibility-test", "version": "1"},
				}, "")
				listed := rpc("tools/list", map[string]any{}, "")
				var tools struct{ Tools []struct{ Name string } }
				if err := json.Unmarshal(listed["result"], &tools); err != nil || len(tools.Tools) == 0 {
					t.Fatalf("missing tools: %s (%v)", listed["result"], err)
				}
				for _, secret := range []string{"test-only", "", "wrong"} {
					called := rpc("tools/call", map[string]any{"name": "room", "arguments": map[string]any{}}, secret)
					var result struct {
						IsError bool `json:"isError"`
					}
					if err := json.Unmarshal(called["result"], &result); err != nil {
						t.Fatal(err)
					}
					if result.IsError != (secret != "test-only") {
						t.Fatalf("organiser authorization mismatch: %s", called["result"])
					}
				}
			})
		}
	}
}
