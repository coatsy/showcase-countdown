package main

import (
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestMCPAllowedHostProtocol(t *testing.T) {
	t.Setenv("MCP_ALLOWED_HOSTS", "mcp.cauldnz.org,mcp.cauldnz.org:443")
	for _, host := range []string{"mcp.cauldnz.org", "mcp.cauldnz.org:443"} {
		for _, path := range []string{"/mcp", "/mcp/1234"} {
			t.Run(host+path, func(t *testing.T) { testMCPHostProtocol(t, host, path) })
		}
	}
}

func testMCPHostProtocol(t *testing.T, host, path string) {
	app := &App{fleet: NewFleet(NewBus(10)), policy: NewPolicy(time.Time{}), secret: "test-only"}
	app.fleet.devices["test-device"] = &Device{ID: "test-device", Code: 1234, Team: "Host Test"}
	handler := app.mcpHandler()
	mux := http.NewServeMux()
	mux.Handle("/mcp", handler)
	mux.HandleFunc("/mcp/{code}", func(w http.ResponseWriter, r *http.Request) {
		r.Header.Set(headerCode, r.PathValue("code"))
		handler.ServeHTTP(w, r)
	})
	claimCode := ""
	rpc := func(method, params, secret string) json.RawMessage {
		t.Helper()
		req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(
			`{"jsonrpc":"2.0","id":1,"method":"`+method+`","params":`+params+`}`))
		req.Host = host
		req = req.WithContext(context.WithValue(req.Context(), http.LocalAddrContextKey, &net.TCPAddr{IP: net.ParseIP("127.0.0.1"), Port: 8090}))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Accept", "application/json, text/event-stream")
		req.Header.Set("Mcp-Protocol-Version", "2025-11-25")
		req.Header.Set(headerSecret, secret)
		req.Header.Set(headerCode, claimCode)
		req.Header.Set("Forwarded", "host=evil.example;proto=https")
		req.Header.Set("X-Forwarded-Host", "evil.example")
		response := httptest.NewRecorder()
		mux.ServeHTTP(response, req)
		if response.Code != http.StatusOK {
			t.Fatalf("%s: HTTP %d: %s", method, response.Code, response.Body.String())
		}
		raw := response.Body.Bytes()
		for _, line := range strings.Split(string(raw), "\n") {
			if data, ok := strings.CutPrefix(line, "data:"); ok {
				raw = []byte(strings.TrimSpace(data))
				break
			}
		}
		var envelope struct {
			Result json.RawMessage `json:"result"`
			Error  json.RawMessage `json:"error"`
		}
		if err := json.Unmarshal(raw, &envelope); err != nil {
			t.Fatal(err)
		}
		if envelope.Error != nil || envelope.Result == nil {
			t.Fatalf("%s: invalid RPC response: %s", method, raw)
		}
		return envelope.Result
	}
	initialized := rpc("initialize", `{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"host-test","version":"1"}}`, "")
	var init struct{ ProtocolVersion string }
	if err := json.Unmarshal(initialized, &init); err != nil || init.ProtocolVersion != "2025-11-25" {
		t.Fatalf("invalid initialize result: %s", initialized)
	}
	listed := rpc("tools/list", `{}`, "")
	var tools struct{ Tools []struct{ Name string } }
	if err := json.Unmarshal(listed, &tools); err != nil {
		t.Fatal(err)
	}
	found := false
	for _, tool := range tools.Tools {
		found = found || tool.Name == "room"
	}
	if !found {
		t.Fatal("room missing from tools/list")
	}
	for _, secret := range []string{"test-only", "", "wrong"} {
		called := rpc("tools/call", `{"name":"room","arguments":{}}`, secret)
		var result struct {
			IsError bool `json:"isError"`
			Content []struct{ Text string }
		}
		if err := json.Unmarshal(called, &result); err != nil {
			t.Fatal(err)
		}
		if result.IsError != (secret != "test-only") || len(result.Content) == 0 {
			t.Fatal("room authorization or content mismatch")
		}
		if !result.IsError {
			var room map[string]json.RawMessage
			if err := json.Unmarshal([]byte(result.Content[0].Text), &room); err != nil ||
				room["devices"] == nil || string(room["manual_lock"]) != "false" {
				t.Fatal("room did not return the actual read-only fleet snapshot")
			}
		}
	}
	for _, code := range []string{"", "9999", "1234"} {
		claimCode = code
		called := rpc("tools/call", `{"name":"status","arguments":{}}`, "")
		var result struct {
			IsError bool `json:"isError"`
		}
		if err := json.Unmarshal(called, &result); err != nil {
			t.Fatal(err)
		}
		if result.IsError != (path == "/mcp" && code != "1234") {
			t.Fatal("team authorization changed")
		}
	}
}

func TestMCPHostProtection(t *testing.T) {
	for _, config := range []string{"", "mcp.cauldnz.org", " MCP.CAULDNZ.ORG , other.example:8443 ", "mcp.cauldnz.org:443", "192.0.2.1,[2001:db8::1]:8443,k.example"} {
		t.Run("config="+config, func(t *testing.T) {
			t.Setenv("MCP_ALLOWED_HOSTS", config)
			handler := (&App{}).mcpHandler()
			public := strings.Contains(strings.ToLower(config), "mcp.cauldnz.org") && !strings.Contains(config, ":443")
			for _, tc := range []struct {
				host, local string
				allowed     bool
			}{
				{"mcp.cauldnz.org", "127.0.0.1", public},
				{"MCP.CAULDNZ.ORG", "127.0.0.1", public},
				{"mcp.cauldnz.org", "::1", public},
				{"mcp.cauldnz.org", "192.168.8.1", public},
				{"mcp.cauldnz.org", "", public},
				{"mcp.cauldnz.org:443", "127.0.0.1", config == "mcp.cauldnz.org:443"},
				{"mcp.cauldnz.org:8090", "127.0.0.1", false},
				{"other.example:8443", "127.0.0.1", strings.Contains(config, "other.example:8443")},
				{"other.example", "127.0.0.1", false},
				{"other.example:443", "127.0.0.1", false},
				{"192.0.2.1", "127.0.0.1", strings.Contains(config, "192.0.2.1")},
				{"192.0.2.1:443", "127.0.0.1", false},
				{"[2001:db8::1]:8443", "127.0.0.1", strings.Contains(config, "[2001:db8::1]:8443")},
				{"[2001:db8::1]", "127.0.0.1", false},
				{"\u212a.example", "127.0.0.1", false}, // Unicode must not fold to allowed k.example.
				{"localhost", "127.0.0.1", true},
				{"localhost:8090", "127.0.0.1", true},
				{"127.0.0.1:8090", "127.0.0.1", true},
				{"127.0.0.2", "127.0.0.1", true},
				{"[::1]", "::1", true},
				{"[::1]:8090", "::1", true},
				{"[::ffff:127.0.0.1]:8090", "127.0.0.1", true},
				{"192.168.8.1:8080", "192.168.8.1", true},
				{"10.0.0.1", "10.0.0.1", true},
				{"[fd00::1]:8080", "fd00::1", true},
				{"192.168.8.2:8080", "192.168.8.1", false},
				{"192.168.8.1:8080", "127.0.0.1", false},
				{"8.8.8.8", "192.168.8.1", false},
				{"evil.example", "127.0.0.1", false},
				{"evil.example", "192.168.8.1", false},
				{"evil.example", "", false},
				{"mcp.cauldnz.org.evil.example", "127.0.0.1", false},
				{"evilmcp.cauldnz.org", "127.0.0.1", false},
				{"sub.mcp.cauldnz.org", "127.0.0.1", false},
				{"localhost.evil.example", "127.0.0.1", false},
				{"127.0.0.1.evil.example", "127.0.0.1", false},
				{"mcp.cauldnz.org.", "127.0.0.1", false},
				{"mcp.cauldnz.org:", "127.0.0.1", false},
				{"mcp.cauldnz.org:0", "127.0.0.1", false},
				{"mcp.cauldnz.org:65536", "127.0.0.1", false},
				{"mcp.cauldnz.org:443:80", "127.0.0.1", false},
				{"mcp.cauldnz.org@evil.example", "127.0.0.1", false},
				{"mcp.cauldnz.org,evil.example", "127.0.0.1", false},
				{" mcp.cauldnz.org", "127.0.0.1", false},
				{"mcp.cauldnz.org/path", "127.0.0.1", false},
				{"localhost:bad", "127.0.0.1", false},
				{"localhost:65536", "127.0.0.1", false},
				{"[localhost]:8090", "127.0.0.1", false},
				{"", "127.0.0.1", false},
			} {
				t.Run(tc.host+"@"+tc.local, func(t *testing.T) {
					for _, method := range []string{http.MethodPost, http.MethodGet} {
						req := httptest.NewRequest(method, "/mcp", strings.NewReader(
							`{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"host-test","version":"1"}}}`))
						req.Host = tc.host
						if tc.local != "" {
							req = req.WithContext(context.WithValue(req.Context(), http.LocalAddrContextKey,
								&net.TCPAddr{IP: net.ParseIP(tc.local), Port: 8090}))
						}
						req.Header.Set("Content-Type", "application/json")
						req.Header.Set("Accept", "application/json, text/event-stream")
						req.Header.Set("Forwarded", `host=mcp.cauldnz.org;proto=https`)
						req.Header.Set("X-Forwarded-Host", "mcp.cauldnz.org")
						req.Header.Set("X-Forwarded-Proto", "https")
						response := httptest.NewRecorder()
						handler.ServeHTTP(response, req)
						want := http.StatusForbidden
						if tc.allowed {
							want = http.StatusOK
							if method == http.MethodGet {
								want = http.StatusMethodNotAllowed // Not a handshake.
							}
						}
						if response.Code != want {
							t.Fatalf("%s: HTTP %d, want %d: %s", method, response.Code, want, response.Body.String())
						}
						if req.Host != tc.host {
							t.Fatal("handler rewrote Host")
						}
					}
				})
			}
		})
	}
}

func TestMCPInvalidAllowedHosts(t *testing.T) {
	for _, config := range []string{
		"*", "*.cauldnz.org", ".cauldnz.org", "https://mcp.cauldnz.org",
		"mcp.cauldnz.org/", "mcp.cauldnz.org?q=x", "mcp.cauldnz.org#fragment",
		"user@mcp.cauldnz.org", "mcp.cauldnz.org,",
		"mcp.cauldnz.org,,other.example", "mcp.cauldnz.org other.example",
		"mcp.cauldnz.org:", "mcp.cauldnz.org:0", "mcp.cauldnz.org:65536",
		"mcp.cauldnz.org:-1", "mcp.cauldnz.org:+443", "mcp.cauldnz.org:https",
		"mcp.cauldnz.org:443:80", "mcp.cauldnz.org.", "bad..example",
		"-bad.example", "bad-.example", "bad_name.example",
		"[mcp.cauldnz.org]:443", "[mcp.cauldnz.org]", "::1", "mcp%2ecauldnz.org",
		"\u212a.example", "mcp.cauldnz.org\r\nHost:evil.example",
		strings.Repeat("a", 64) + ".example", strings.Repeat("a.", 127) + "a",
	} {
		t.Run(config, func(t *testing.T) {
			t.Setenv("MCP_ALLOWED_HOSTS", config)
			defer func() {
				message, ok := recover().(string)
				if !ok || !strings.HasPrefix(message, "MCP_ALLOWED_HOSTS entry ") {
					t.Fatal("invalid host configuration did not fail with a configuration error")
				}
			}()
			(&App{}).mcpHandler()
		})
	}
}
