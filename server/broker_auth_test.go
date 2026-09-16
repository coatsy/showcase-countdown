package main

import (
	"bytes"
	"crypto/rand"
	"encoding/json"
	"log/slog"
	"net"
	"strings"
	"testing"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	mqttsrv "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/packets"
)

func brokerTestUsers(t *testing.T, users map[string]string) string {
	t.Helper()
	raw, err := json.Marshal(users)
	if err != nil {
		t.Fatal("could not encode test credentials")
	}
	return string(raw)
}

func TestBrokerWSConfig(t *testing.T) {
	password := rand.Text() + rand.Text()
	users := brokerTestUsers(t, map[string]string{"abcdef": password})
	for _, tc := range []struct {
		name, listen, users string
		valid               bool
	}{
		{"disabled", "", "", true},
		{"IPv4", "127.0.0.1:8092", users, true},
		{"IPv6", "[::1]:8092", users, true},
		{"mapped-loopback", "[::ffff:127.0.0.1]:8092", users, true},
		{"missing-listener", "", users, false},
		{"missing-users", "127.0.0.1:8092", "", false},
		{"wildcard", ":8092", users, false},
		{"any-IPv4", "0.0.0.0:8092", users, false},
		{"any-IPv6", "[::]:8092", users, false},
		{"LAN", "192.168.8.1:8092", users, false},
		{"DNS", "localhost:8092", users, false},
		{"public", "mqtt.cauldnz.org:8092", users, false},
		{"missing-port", "127.0.0.1", users, false},
		{"zero-port", "127.0.0.1:0", users, false},
		{"large-port", "127.0.0.1:65536", users, false},
		{"named-port", "127.0.0.1:http", users, false},
		{"URL", "ws://127.0.0.1:8092", users, false},
		{"path", "127.0.0.1:8092/mqtt", users, false},
		{"empty-object", "127.0.0.1:8092", `{}`, false},
		{"null", "127.0.0.1:8092", `null`, false},
		{"array", "127.0.0.1:8092", `[]`, false},
		{"malformed", "127.0.0.1:8092", `{"abcdef":`, false},
		{"trailing-JSON", "127.0.0.1:8092", users + `{}`, false},
		{"trailing-garbage", "127.0.0.1:8092", users + `x`, false},
		{"duplicate-ID", "127.0.0.1:8092", strings.TrimSuffix(users, "}") + "," + strings.TrimPrefix(users, "{"), false},
		{"shared-password", "127.0.0.1:8092", brokerTestUsers(t, map[string]string{"abcdef": password, "123456": password}), false},
		{"null-password", "127.0.0.1:8092", `{"abcdef":null}`, false},
		{"numeric-password", "127.0.0.1:8092", `{"abcdef":123}`, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			config, err := parseBrokerWSConfig(tc.listen, tc.users)
			if (err == nil) != tc.valid {
				t.Fatalf("configuration validity mismatch: valid=%t", tc.valid)
			}
			if err != nil && strings.Contains(err.Error(), password) {
				t.Fatal("configuration error disclosed a secret")
			}
			if tc.valid && tc.listen != "" && (config.listen != tc.listen || len(config.users) != 1) {
				t.Fatal("valid configuration lost listener or identity")
			}
		})
	}
	for _, id := range []string{"", "abcde", "abcdef0", "ABCDEF", "abcdeg", "abc/ef", "abc+ef", "abc#ef"} {
		if _, err := parseBrokerWSConfig("127.0.0.1:8092", brokerTestUsers(t, map[string]string{id: password})); err == nil {
			t.Fatal("invalid device ID accepted")
		}
	}
	for _, weak := range []string{"", "short", strings.Repeat("a", 32), strings.Repeat("abcd", 32), password + " ", password + "\n", password + "\u00e9", strings.Repeat(password, 3)} {
		if _, err := parseBrokerWSConfig("127.0.0.1:8092", brokerTestUsers(t, map[string]string{"abcdef": weak})); err == nil {
			t.Fatal("weak or malformed password accepted")
		}
	}
}

func TestBrokerWSProtocol(t *testing.T) {
	passwordA, passwordB := rand.Text()+rand.Text(), rand.Text()+rand.Text()
	// Mochi's native Websocket listener cannot report an allocated :0 port.
	// Reserve an ephemeral loopback port, then release it immediately before start.
	reservation, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := reservation.Addr().String()
	config, err := parseBrokerWSConfig(address, brokerTestUsers(t, map[string]string{
		"abcdef": passwordA, "123456": passwordB,
	}))
	if err != nil {
		_ = reservation.Close()
		t.Fatal("test configuration failed")
	}
	_ = reservation.Close()
	broker, err := startEmbeddedBroker("127.0.0.1:0", config)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = broker.Close() })
	lan, _ := broker.Listeners.Get("tcp4")
	ws, ok := broker.Listeners.Get(brokerWSListener)
	if !ok || ws.Address() != address || ws.Protocol() != "ws" {
		t.Fatal("missing native loopback WebSocket listener")
	}
	deadline := time.Now().Add(3 * time.Second)
	for {
		conn, err := net.DialTimeout("tcp", address, 100*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("WebSocket listener did not start")
		}
		time.Sleep(10 * time.Millisecond)
	}
	endpoint := "ws://" + address + "/mqtt"
	options := func(endpoint, id, user, password string) *mqtt.ClientOptions {
		return mqtt.NewClientOptions().AddBroker(endpoint).SetClientID(id).
			SetUsername(user).SetPassword(password).SetAutoReconnect(false).
			SetConnectTimeout(time.Second).SetOrderMatters(true)
	}
	connect := func(opts *mqtt.ClientOptions, allowed bool) mqtt.Client {
		t.Helper()
		client := mqtt.NewClient(opts)
		token := client.Connect()
		if !token.WaitTimeout(3 * time.Second) {
			t.Fatal("MQTT connection timed out")
		}
		if token.Error() == nil {
			t.Cleanup(func() { client.Disconnect(100) })
		}
		if (token.Error() == nil) != allowed {
			t.Fatalf("MQTT authentication mismatch: allowed=%t", allowed)
		}
		return client
	}
	receive := func(ch <-chan string, want string) {
		t.Helper()
		select {
		case got := <-ch:
			if got != want {
				t.Fatalf("received topic %q, want %q", got, want)
			}
		case <-time.After(3 * time.Second):
			t.Fatalf("no delivery for %s", want)
		}
	}
	observer := connect(options("tcp://"+lan.Address(), "lan-observer", "", ""), true)
	seen := make(chan string, 64)
	relayWaitToken(t, observer.Subscribe("showcase/#", 1, func(_ mqtt.Client, m mqtt.Message) { seen <- m.Topic() }))
	relayWaitToken(t, observer.Subscribe("$SYS/test", 1, func(_ mqtt.Client, m mqtt.Message) { seen <- m.Topic() }))
	deviceA := connect(options(endpoint, "stick-abcdef", "abcdef", passwordA), true)
	deviceB := connect(options(endpoint, "stick-123456", "123456", passwordB), true)
	for _, tc := range []struct{ name, id, user, password string }{
		{"anonymous", "stick-abcdef", "", ""},
		{"missing-password", "stick-abcdef", "abcdef", ""},
		{"wrong-password", "stick-abcdef", "abcdef", passwordB},
		{"unknown-user", "stick-fedcba", "fedcba", passwordA},
		{"other-device-ID", "stick-123456", "abcdef", passwordA},
		{"server-ID", "showcase-server", "abcdef", passwordA},
		{"arbitrary-ID", "anything", "abcdef", passwordA},
	} {
		t.Run(tc.name, func(t *testing.T) {
			connect(options(endpoint, tc.id, tc.user, tc.password), false)
			if !deviceA.IsConnectionOpen() || !deviceB.IsConnectionOpen() {
				t.Fatal("failed authentication stole an existing client ID")
			}
		})
	}
	aCommands, bCommands := make(chan string, 8), make(chan string, 8)
	relayWaitToken(t, deviceA.Subscribe("showcase/dev/abcdef/cmd/#", 1, func(_ mqtt.Client, m mqtt.Message) { aCommands <- m.Topic() }))
	relayWaitToken(t, deviceA.Subscribe("showcase/all/cmd/#", 1, func(_ mqtt.Client, m mqtt.Message) { aCommands <- m.Topic() }))
	relayWaitToken(t, deviceB.Subscribe("showcase/dev/123456/cmd/#", 1, func(_ mqtt.Client, m mqtt.Message) { bCommands <- m.Topic() }))
	for _, topic := range []string{"showcase/dev/abcdef/state", "showcase/dev/abcdef/event/button"} {
		relayWaitToken(t, deviceA.Publish(topic, 1, false, "local-test"))
		receive(seen, topic)
	}
	for _, topic := range []string{"showcase/dev/abcdef/cmd/display", "showcase/all/cmd/display"} {
		relayWaitToken(t, observer.Publish(topic, 1, false, "local-test"))
		receive(aCommands, topic)
		receive(seen, topic)
	}
	relayWaitToken(t, observer.Publish("showcase/dev/123456/cmd/display", 1, false, "local-test"))
	receive(bCommands, "showcase/dev/123456/cmd/display")
	receive(seen, "showcase/dev/123456/cmd/display")

	for _, topic := range []string{
		"#", "+/#", "showcase/#", "showcase/+/abcdef/cmd/#", "showcase/dev/+/cmd/#",
		"showcase/dev/123456/cmd/#", "showcase/dev/abcdef/#", "showcase/dev/abcdef/state",
		"showcase/all/#", "showcase/bridge/#", "$SYS/#", "$share/group/showcase/all/cmd/#",
		"showcase/dev/abcdef/cmd/#/extra", "showcase/dev/abcdef/cmd/+",
	} {
		t.Run("subscribe/"+topic, func(t *testing.T) {
			token := deviceA.Subscribe(topic, 1, nil)
			if !token.WaitTimeout(3 * time.Second) {
				t.Fatal("subscription timed out")
			}
			// Paho strips $share/group/ from keys in the SUBACK result map.
			if token.Error() == nil {
				result := token.(*mqtt.SubscribeToken).Result()
				if len(result) != 1 {
					t.Fatal("missing SUBACK result")
				}
				for _, code := range result {
					if code != 0x80 {
						t.Fatal("forbidden subscription was not rejected")
					}
				}
			}
			client, ok := broker.Clients.Get("stick-abcdef")
			if !ok {
				t.Fatal("device disappeared during subscribe denial")
			}
			if _, exists := client.State.Subscriptions.Get(topic); exists {
				t.Fatal("forbidden subscription stored")
			}
		})
	}

	// Mochi silently drops reserved $SYS publications before its ACL hook.
	// A subsequent acknowledged state publish is an ordered processing barrier.
	relayWaitToken(t, deviceA.Publish("$SYS/test", 0, true, "forbidden"))
	relayWaitToken(t, deviceA.Publish("showcase/dev/abcdef/state", 1, false, "barrier"))
	receive(seen, "showcase/dev/abcdef/state")
	if len(broker.Topics.Messages("$SYS/test")) != 0 {
		t.Fatal("system-topic publication was retained")
	}
	deviceA.Disconnect(100)
	for _, topic := range []string{
		"showcase/dev/123456/state", "showcase/dev/123456/event/button",
		"showcase/dev/abcdef/cmd/display", "showcase/all/cmd/display",
		"showcase/dev/abcdef/state/extra", "showcase/bridge/state",
	} {
		t.Run("publish/"+topic, func(t *testing.T) {
			lost := make(chan struct{}, 1)
			opts := options(endpoint, "stick-abcdef", "abcdef", passwordA)
			opts.SetConnectionLostHandler(func(mqtt.Client, error) { lost <- struct{}{} })
			client := connect(opts, true)
			client.Publish(topic, 1, true, "forbidden")
			select {
			case <-lost: // MQTT 3.1.1 rejects unauthorized QoS 1 publication by disconnecting.
			case <-time.After(3 * time.Second):
				t.Fatal("forbidden publisher was not disconnected")
			}
			if len(broker.Topics.Messages(topic)) != 0 {
				t.Fatal("forbidden publication was retained")
			}
		})
	}
	for _, topic := range []string{"showcase/all/cmd/display", "showcase/dev/123456/state", "showcase/bridge/state", "$SYS/test"} {
		t.Run("will/"+topic, func(t *testing.T) {
			opts := options(endpoint, "stick-abcdef", "abcdef", passwordA)
			opts.SetWill(topic, "forbidden", 1, true)
			connect(opts, false)
		})
	}
	opts := options(endpoint, "stick-abcdef", "abcdef", passwordA)
	opts.SetWill("showcase/dev/abcdef/state", "offline", 1, true)
	connect(opts, true).Disconnect(100)

	// A public reconnect may replace its own LAN device, but must not inherit
	// unrestricted LAN subscriptions or unacknowledged messages.
	legacy := connect(options("tcp://"+lan.Address(), "stick-abcdef", "", "").SetCleanSession(false), true)
	relayWaitToken(t, legacy.Subscribe("showcase/#", 1, nil))
	connect(options(endpoint, "stick-abcdef", "abcdef", passwordA).SetCleanSession(false), false)
	if !legacy.IsConnectionOpen() {
		t.Fatal("unsafe session inheritance attempt evicted LAN device")
	}
	connect(options(endpoint, "stick-abcdef", "abcdef", passwordA), true)
	client, _ := broker.Clients.Get("stick-abcdef")
	if _, exists := client.State.Subscriptions.Get("showcase/#"); exists {
		t.Fatal("clean public reconnect inherited LAN subscription")
	}
	// Any leaked denied publish reaches this ordered observer before the marker.
	relayWaitToken(t, observer.Publish("showcase/test-complete", 1, false, "done"))
	receive(seen, "showcase/test-complete")
	select {
	case <-aCommands:
		t.Fatal("cross-device command delivered")
	default:
	}
}

func TestBrokerListenerPolicy(t *testing.T) {
	broker, err := startEmbeddedBroker("127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer broker.Close()
	if broker.Listeners.Len() != 1 {
		t.Fatal("public listener enabled without configuration")
	}
	hook := &brokerAuth{server: broker}
	for _, listener := range []string{"tcp4", brokerWSListener, "unknown", ""} {
		client := &mqttsrv.Client{Net: mqttsrv.ClientConnection{Listener: listener}}
		if hook.OnConnectAuthenticate(client, packets.Packet{}) != (listener == "tcp4") ||
			hook.OnACLCheck(client, "#", false) != (listener == "tcp4") ||
			hook.OnACLCheck(client, "$SYS/test", true) != (listener == "tcp4") {
			t.Fatal("listener ACL did not fail closed")
		}
	}
}

func TestBrokerLogRedaction(t *testing.T) {
	var output bytes.Buffer
	logger := slog.New(slog.NewTextHandler(&output, &slog.HandlerOptions{ReplaceAttr: brokerLogAttribute}))
	logger.Warn("error processing packet", "error", "rejected", "pk", packets.Packet{
		Connect: packets.ConnectParams{Password: []byte(rand.Text())},
	})
	if strings.Contains(output.String(), "pk=") || !strings.Contains(output.String(), "error=rejected") {
		t.Fatal("packet redaction lost diagnostics or disclosed raw packets")
	}
}
