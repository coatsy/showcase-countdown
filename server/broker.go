package main

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"log/slog"
	"net"
	"os"
	"strconv"
	"strings"

	mqttsrv "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/mochi-mqtt/server/v2/packets"
)

const brokerWSListener = "mqtt-ws"

type brokerWSConfig struct {
	listen string
	users  map[string][sha256.Size]byte
}

// Secrets must be independently generated, 32..128 printable non-space ASCII
// characters. Keep only their hashes; never include input in configuration errors.
func parseBrokerWSConfig(listen, users string) (brokerWSConfig, error) {
	var config brokerWSConfig
	if listen == "" && users == "" {
		return config, nil
	}
	host, port, err := net.SplitHostPort(listen)
	n, portErr := strconv.ParseUint(port, 10, 16)
	if err != nil || portErr != nil || n == 0 || !net.ParseIP(host).IsLoopback() {
		return config, fmt.Errorf("MQTT_WS_LISTEN must be a literal loopback IP and port in 1..65535")
	}
	invalidUsers := fmt.Errorf("MQTT_WS_USERS must be a nonempty JSON object of unique six-lowercase-hex device IDs and distinct strong passwords (32..128 printable non-space ASCII characters, at least 8 distinct)")
	config.users = make(map[string][sha256.Size]byte)
	hashes := make(map[[sha256.Size]byte]bool)
	decoder := json.NewDecoder(strings.NewReader(users))
	if token, err := decoder.Token(); err != nil || token != json.Delim('{') {
		return brokerWSConfig{}, invalidUsers
	}
	for decoder.More() {
		token, err := decoder.Token()
		id, ok := token.(string)
		if err != nil || !ok || len(id) != 6 || strings.Trim(id, "0123456789abcdef") != "" {
			return brokerWSConfig{}, invalidUsers
		}
		var password string
		if err := decoder.Decode(&password); err != nil || len(password) < 32 || len(password) > 128 {
			return brokerWSConfig{}, invalidUsers
		}
		distinct := make(map[byte]bool)
		for i := range len(password) {
			if password[i] < '!' || password[i] > '~' {
				return brokerWSConfig{}, invalidUsers
			}
			distinct[password[i]] = true
		}
		hash := sha256.Sum256([]byte(password))
		if _, exists := config.users[id]; exists || hashes[hash] || len(distinct) < 8 {
			return brokerWSConfig{}, invalidUsers
		}
		config.users[id], hashes[hash] = hash, true
	}
	if token, err := decoder.Token(); err != nil || token != json.Delim('}') || len(config.users) == 0 {
		return brokerWSConfig{}, invalidUsers
	}
	if _, err := decoder.Token(); err != io.EOF {
		return brokerWSConfig{}, invalidUsers
	}
	config.listen = listen
	return config, nil
}

// Mochi combines authentication/ACL hooks with OR, so this replaces AllowHook.
// Only the existing tcp4 listener is trusted; unknown listeners fail closed.
type brokerAuth struct {
	mqttsrv.HookBase
	server *mqttsrv.Server
	users  map[string][sha256.Size]byte
}

func (*brokerAuth) ID() string { return "listener-auth" }

func (*brokerAuth) Provides(b byte) bool {
	return b == mqttsrv.OnConnectAuthenticate || b == mqttsrv.OnACLCheck
}

func (h *brokerAuth) OnConnectAuthenticate(cl *mqttsrv.Client, pk packets.Packet) bool {
	if cl.Net.Listener == "tcp4" {
		return true
	}
	id := string(pk.Connect.Username)
	expected, exists := h.users[id]
	actual := sha256.Sum256(pk.Connect.Password)
	if cl.Net.Listener != brokerWSListener || !exists ||
		!pk.Connect.UsernameFlag || !pk.Connect.PasswordFlag ||
		cl.ID != "stick-"+id || subtle.ConstantTimeCompare(actual[:], expected[:]) != 1 {
		return false
	}
	// Wills bypass the normal publish ACL in Mochi. Validate before accepting
	// CONNECT, which also runs before the broker can take over a client ID.
	if pk.Connect.WillFlag && !h.OnACLCheck(cl, pk.Connect.WillTopic, true) {
		return false
	}
	// A persistent LAN session may contain unrestricted subscriptions/inflight
	// messages. Require a clean reconnect when crossing into the public listener.
	if existing, ok := h.server.Clients.Get(cl.ID); ok &&
		existing.Net.Listener != brokerWSListener && !pk.Connect.Clean {
		return false
	}
	return true
}

func (h *brokerAuth) OnACLCheck(cl *mqttsrv.Client, topic string, write bool) bool {
	if cl.Net.Listener == "tcp4" {
		return true
	}
	id := string(cl.Properties.Username)
	if _, exists := h.users[id]; cl.Net.Listener != brokerWSListener || !exists || cl.ID != "stick-"+id {
		return false
	}
	device := topicPrefix + "dev/" + id + "/"
	if write {
		return topic == device+"state" || topic == device+"event/button"
	}
	for _, prefix := range []string{device + "cmd/", topicPrefix + "all/cmd/"} {
		if subtopic, ok := strings.CutPrefix(topic, prefix); ok {
			return subtopic == "#" || (subtopic != "" && !strings.ContainsAny(subtopic, "+#"))
		}
	}
	return false
}

// Mochi logs rejected packets, including a second CONNECT's password. Keep the
// diagnostic metadata, never the raw packet or payload.
func brokerLogAttribute(_ []string, attr slog.Attr) slog.Attr {
	if attr.Key == "pk" {
		return slog.Attr{}
	}
	return attr
}

// startEmbeddedBroker runs an MQTT broker inside this process. GL.iNet's
// package feeds do not carry Mosquitto, so the router gets one binary that is
// broker, MCP server and dashboard together. TCP remains anonymous on the trusted
// LAN. Optional WS is loopback-only behind the TLS-terminating Cloudflare tunnel.
func startEmbeddedBroker(addr string, public ...brokerWSConfig) (*mqttsrv.Server, error) {
	// ":1883" would bind IPv6-only on some kernels (seen on the GL-MT3000),
	// leaving IPv4 clients, including our own loopback connection, unable to
	// reach it. The sticks are IPv4, so bind IPv4 explicitly.
	if strings.HasPrefix(addr, ":") {
		addr = "0.0.0.0" + addr
	}
	srv := mqttsrv.New(&mqttsrv.Options{
		InlineClient: false,
		Logger: slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{
			ReplaceAttr: brokerLogAttribute,
		})),
	})
	var ws brokerWSConfig
	if len(public) > 0 {
		ws = public[0]
	}
	if err := srv.AddHook(&brokerAuth{server: srv, users: ws.users}, nil); err != nil {
		return nil, err
	}
	ln, err := net.Listen("tcp4", addr)
	if err != nil {
		return nil, err
	}
	if err := srv.AddListener(listeners.NewNet("tcp4", ln)); err != nil {
		_ = ln.Close()
		return nil, err
	}
	if ws.listen != "" {
		if err := srv.AddListener(listeners.NewWebsocket(listeners.Config{
			ID: brokerWSListener, Address: ws.listen,
		})); err != nil {
			_ = srv.Close()
			return nil, err
		}
	}
	if err := srv.Serve(); err != nil {
		_ = srv.Close()
		return nil, err
	}
	log.Printf("broker: embedded MQTT listening on %s", addr)
	if ws.listen != "" {
		log.Printf("broker: authenticated MQTT WebSocket on %s/mqtt (TLS terminates at tunnel)", ws.listen)
	}
	return srv, nil
}
