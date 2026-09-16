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

func TestFleetOTAMetadata(test *testing.T) {
	fleet := NewFleet(NewBus(10))
	fleet.onState("533db4", []byte(`{"online":true,"team":"Coatsy","ip":"192.168.8.128","ota":true,"chip":"esp32","fw":"ota-test","image_md5":"0123456789abcdef0123456789abcdef"}`))
	app := &App{fleet: fleet, policy: NewPolicy(time.Time{})}
	mux := http.NewServeMux()
	app.dashboardRoutes(mux)
	response := httptest.NewRecorder()
	mux.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/api/fleet", nil))
	var snapshot struct {
		Devices []Device `json:"devices"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil {
		test.Fatal(err)
	}
	if len(snapshot.Devices) != 1 {
		test.Fatalf("unexpected fleet: %+v", snapshot)
	}
	device := snapshot.Devices[0]
	if !device.OTA || device.IP != "192.168.8.128" || device.Chip != "esp32" ||
		device.FW != "ota-test" || device.ImageMD5 != "0123456789abcdef0123456789abcdef" {
		test.Fatalf("OTA metadata missing: %+v", device)
	}
	fleet.onState("533db4", []byte(`{"online":false}`))
	if fleet.Snapshot()[0].OTA {
		test.Fatal("offline device advertised OTA readiness")
	}
	fleet.onState("533db4", []byte(`{"online":true,"fw":"legacy"}`))
	device = fleet.Snapshot()[0]
	if device.OTA || device.IP != "" || device.Chip != "" || device.ImageMD5 != "" {
		test.Fatalf("legacy state retained stale OTA metadata: %+v", device)
	}
}

func TestDashboardOTAUpdateRoute(t *testing.T) {
	bus := NewBus(10)
	fleet := NewFleet(bus)
	fleet.onState("device-1", []byte(`{"online":true,"team":"Coatsy","ip":"192.168.8.10","ota":true}`))
	fleet.onState("device-2", []byte(`{"online":false,"team":"Other","ip":"192.168.8.11","ota":true}`))
	var seen []string
	app := &App{
		fleet:     fleet,
		policy:    NewPolicy(time.Time{}),
		bus:       bus,
		secret:    "test-secret",
		otaRunner: func(targets []string) error { seen = append([]string(nil), targets...); return nil },
	}
	mux := http.NewServeMux()
	app.dashboardRoutes(mux)

	response := httptest.NewRecorder()
	request := httptest.NewRequest(http.MethodPost, "/api/organiser/ota?target=all", nil)
	request.Header.Set(headerSecret, "test-secret")
	mux.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("fleet OTA update returned HTTP %d: %s", response.Code, response.Body.String())
	}
	if len(seen) != 1 || seen[0] != "192.168.8.10" {
		t.Fatalf("fleet OTA update sent the wrong devices: %#v", seen)
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
