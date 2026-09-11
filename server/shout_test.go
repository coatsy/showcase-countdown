package main

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"github.com/modelcontextprotocol/go-sdk/mcp"
)

type shoutPublisher struct {
	mqtt.Client
	topics   []string
	payloads [][]byte
	failAt   int
}

type shoutToken struct {
	mqtt.Token
	err error
}

func (t shoutToken) Wait() bool   { return true }
func (t shoutToken) Error() error { return t.err }
func (p *shoutPublisher) Publish(topic string, _ byte, _ bool, payload any) mqtt.Token {
	p.topics = append(p.topics, topic)
	p.payloads = append(p.payloads, payload.([]byte))
	if len(p.topics) == p.failAt {
		return shoutToken{err: errors.New("publish failed")}
	}
	return shoutToken{}
}

func TestNormalizeColor(t *testing.T) {
	for input, want := range map[string]string{
		"#FF6600": "#FF6600",
		"ff6600":  "#FF6600",
		"red":     "#FF0000",
		"Blue":    "#0000FF",
		"skyblue": "#87CEEB",
		"off":     "#000000",
	} {
		got, err := normalizeColor(input)
		if err != nil {
			t.Fatalf("normalizeColor(%q) returned error: %v", input, err)
		}
		if got != want {
			t.Fatalf("normalizeColor(%q) = %q, want %q", input, got, want)
		}
	}

	if _, err := normalizeColor("bogus"); err == nil {
		t.Fatal("normalizeColor('bogus') should reject unknown colors")
	}
}

func TestShoutNotifications(t *testing.T) {
	for _, scenario := range []string{"accepted", "locked", "invalid", "display failure", "audio failure", "LED failure"} {
		t.Run(scenario, func(t *testing.T) {
			bus := NewBus(10)
			publisher := &shoutPublisher{}
			app := &App{fleet: NewFleet(bus), policy: NewPolicy(time.Time{}), bus: bus}
			app.fleet.client = publisher
			app.fleet.devices["test"] = &Device{ID: "test", Code: 1234, Team: "Test"}
			message := "Hello"
			switch scenario {
			case "locked":
				app.policy.SetManualLock(true)
			case "invalid":
				message = ""
			case "display failure":
				publisher.failAt = 1
			case "audio failure":
				publisher.failAt = 2
			case "LED failure":
				publisher.failAt = 3
			}
			handler := app.mcpHandler()
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				r.Header.Set(headerCode, "1234")
				handler.ServeHTTP(w, r)
			}))
			defer server.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			client := mcp.NewClient(&mcp.Implementation{Name: "shout-test", Version: "1"}, nil)
			session, err := client.Connect(ctx, &mcp.StreamableClientTransport{Endpoint: server.URL}, nil)
			if err != nil {
				t.Fatal(err)
			}
			defer session.Close()
			call := func() *mcp.CallToolResult {
				t.Helper()
				result, err := session.CallTool(ctx, &mcp.CallToolParams{Name: "shout", Arguments: map[string]any{"text": message}})
				if err != nil {
					t.Fatal(err)
				}
				return result
			}
			result := call()
			if result.IsError != (scenario != "accepted") {
				t.Fatalf("unexpected result: %+v", result)
			}
			want := 3
			if scenario == "locked" || scenario == "invalid" {
				want = 0
			} else if scenario == "display failure" {
				want = 1
			} else if scenario == "audio failure" {
				want = 2
			}
			if len(publisher.topics) != want {
				t.Fatalf("published %v; want %d commands", publisher.topics, want)
			}
			if want >= 2 {
				var audio Audio
				if err := json.Unmarshal(publisher.payloads[1], &audio); err != nil {
					t.Fatal(err)
				}
				if want == 3 {
					var led Led
					if err := json.Unmarshal(publisher.payloads[2], &led); err != nil {
						t.Fatal(err)
					}
					if publisher.topics[2] != topicPrefix+"all/cmd/led" || led.Color != "#FFAA00" || led.Mode != "blink" || led.PeriodMs != 600 || led.TTL != 15 {
						t.Fatalf("wrong LED notification: %+v", led)
					}
				}
				if publisher.topics[0] != topicPrefix+"all/cmd/display" || publisher.topics[1] != topicPrefix+"all/cmd/audio" || audio.Jingle != "alarm" || audio.Volume != 200 {
					t.Fatalf("wrong display/audio sequence: %v, %+v", publisher.topics, audio)
				}
			}
			if scenario == "audio failure" {
				body, _ := json.Marshal(result)
				if !strings.Contains(string(body), "shout text sent, but notification sound failed") {
					t.Fatalf("missing partial-failure explanation: %s", body)
				}
			}
			if scenario == "accepted" {
				if !call().IsError || len(publisher.topics) != 3 {
					t.Fatal("cooldown must reject repeat shout without more display, audio or LEDs")
				}
			}
		})
	}
}
