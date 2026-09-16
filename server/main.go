// showcase-server: the MCP server, dashboard and broker bridge for the sticks.
// See docs/plan.md and docs/messaging.md.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func main() {
	var (
		listen  = flag.String("listen", envOr("LISTEN", ":8080"), "HTTP listen address")
		broker  = flag.String("broker", envOr("MQTT_URL", "tcp://127.0.0.1:1883"), "MQTT broker URL")
		secret  = flag.String("secret", os.Getenv("ORGANISER_SECRET"), "organiser secret (env ORGANISER_SECRET)")
		event   = flag.String("event", envOr("EVENT_DATETIME", ""), "event instant, RFC3339 with offset, or unix epoch")
		title   = flag.String("title", envOr("EVENT_NAME", "Countdown"), "event name for the dashboard")
		fakeN   = flag.Int("fake", envInt("FAKE_STICKS", 0), "number of virtual sticks to run")
		bridgeP = flag.String("bridge", os.Getenv("BRIDGE_PORT"), "optional serial relay port, e.g. COM5, /dev/ttyUSB0, or auto for exactly one FTDI adapter")
		embed   = flag.String("embedded-broker", envOr("EMBEDDED_BROKER", ""), "run an MQTT broker in-process on this address, e.g. :1883, and connect to it")
		teams   = flag.String("teams-file", envOr("TEAMS_FILE", ""), "JSON file that keeps team names across restarts, e.g. /etc/showcase/teams.json")
	)
	flag.Parse()
	ws, err := parseBrokerWSConfig(os.Getenv("MQTT_WS_LISTEN"), os.Getenv("MQTT_WS_USERS"))
	if err != nil {
		log.Fatal(err)
	}
	if ws.listen != "" && *embed == "" {
		log.Fatal("MQTT_WS_LISTEN requires EMBEDDED_BROKER or -embedded-broker")
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	epoch, err := parseEvent(*event)
	if err != nil {
		log.Fatalf("bad -event %q: %v", *event, err)
	}

	bus := NewBus(500)
	fleet := NewFleet(bus)
	if n := fleet.LoadTeams(*teams); n > 0 {
		log.Printf("teams: loaded %d from %s", n, *teams)
	}
	fleet.onTeamsChanged = func() { fleet.SaveTeams(*teams) }
	app := &App{
		fleet: fleet, policy: NewPolicy(epoch), bus: bus,
		secret: *secret, title: *title, epoch: epoch,
	}
	mcpHandler := app.mcpHandler()

	if *bridgeP != "" {
		app.bridge = NewSerialBridge(*bridgeP)
		fleet.relay = NewRadioRelay(app.bridge, fleet, app.policy)
	}
	app.armFire()

	if *embed != "" {
		embedded, err := startEmbeddedBroker(*embed, ws)
		if err != nil {
			log.Fatalf("embedded broker: %v", err)
		}
		defer embedded.Close()
		// Connect to our own broker over IPv4 loopback whatever it binds to.
		_, port, _ := strings.Cut(*embed, ":")
		*broker = "tcp://127.0.0.1:" + port
		time.Sleep(200 * time.Millisecond)
	}

	// Start HTTP before the broker connection: fleet.Connect retries until it
	// succeeds, and the dashboard must be reachable even while it does.
	mux := http.NewServeMux()
	mux.Handle("/mcp", mcpHandler)
	// /mcp/<code> lets a team connect with nothing but a URL. The code is
	// copied into the header the tools read, so both forms behave the same.
	mux.HandleFunc("/mcp/{code}", func(w http.ResponseWriter, r *http.Request) {
		if code := r.PathValue("code"); code != "" {
			r.Header.Set(headerCode, code)
		}
		mcpHandler.ServeHTTP(w, r)
	})
	app.dashboardRoutes(mux)

	srv := &http.Server{Addr: *listen, Handler: logRequests(mux)}
	// IPv4 explicitly: on the GL-MT3000 a dual-stack socket never receives
	// IPv4 connections, and every stick and laptop in the room is IPv4.
	ln, err := net.Listen("tcp4", *listen)
	if err != nil {
		log.Fatalf("listen %s: %v", *listen, err)
	}
	go func() {
		log.Printf("listening on %s  (MCP at /mcp/<code>, dashboard at /)", ln.Addr())
		if err := srv.Serve(ln); err != http.ErrServerClosed {
			log.Fatal(err)
		}
	}()
	if !epoch.IsZero() {
		log.Printf("event at %s (%d)", epoch.Format(time.RFC3339), epoch.Unix())
	}
	if *secret == "" {
		log.Printf("warning: no organiser secret; organiser tools are disabled")
	}

	if err := fleet.Connect(*broker); err != nil {
		log.Printf("mqtt: initial connect failed: %v (retrying in background)", err)
	}
	if app.bridge != nil {
		go app.bridge.Run(ctx)
		go fleet.relay.Run(ctx)
	}
	if *fakeN > 0 {
		startFakes(*broker, *fakeN)
		log.Printf("fake fleet: %d virtual sticks", *fakeN)
	}

	<-ctx.Done()
	shutdown, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = srv.Shutdown(shutdown)
}

func (a *App) mcpHandler() http.Handler {
	// Comma-separated exact authorities: a bare hostname permits no port;
	// hostname:port permits only that port. Invalid configuration fails startup.
	allowed := make(map[string]bool)
	if value := strings.TrimSpace(os.Getenv("MCP_ALLOWED_HOSTS")); value != "" {
		for i, entry := range strings.Split(value, ",") {
			entry = strings.TrimSpace(entry)
			if _, err := parseMCPHost(entry); err != nil {
				panic(fmt.Sprintf("MCP_ALLOWED_HOSTS entry %d: %v", i+1, err))
			}
			allowed[strings.ToLower(entry)] = true
		}
	}
	server := a.mcpServer()
	// Identity is per request; stateless transport also accepts client protocol metadata.
	getServer := func(*http.Request) *mcp.Server { return server }
	local := mcp.NewStreamableHTTPHandler(getServer,
		&mcp.StreamableHTTPOptions{Stateless: true})
	// SDK v1.7.0 has no host allowlist. Only exact configured authorities reach
	// this exception handler; all local defaults retain the SDK protection.
	configured := mcp.NewStreamableHTTPHandler(getServer,
		&mcp.StreamableHTTPOptions{Stateless: true, DisableLocalhostProtection: true})
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host, err := parseMCPHost(r.Host)
		if err == nil && allowed[strings.ToLower(r.Host)] {
			configured.ServeHTTP(w, r)
			return
		}
		ip := net.ParseIP(host)
		localIP := false
		if addr, ok := r.Context().Value(http.LocalAddrContextKey).(net.Addr); ok && addr != nil {
			h, _, _ := net.SplitHostPort(addr.String())
			localIP = ip != nil && ip.Equal(net.ParseIP(h))
		}
		// Numeric addresses must be loopback or the actual receiving interface.
		// Never infer authority from Forwarded or X-Forwarded-Host.
		if err != nil || !(host == "localhost" || ip.IsLoopback() || localIP) {
			http.Error(w, fmt.Sprintf("Forbidden: invalid Host header %q", r.Host), http.StatusForbidden)
			return
		}
		local.ServeHTTP(w, r)
	})
}

// parseMCPHost validates a Host authority, not a URL, wildcard, or suffix.
func parseMCPHost(authority string) (string, error) {
	invalid := fmt.Errorf("want an exact hostname or IP, optionally with a port in 1..65535")
	u, err := url.Parse("//" + authority)
	if err != nil || u.Host != authority || authority == "" || strings.HasSuffix(authority, ":") {
		return "", invalid
	}
	host := u.Hostname()
	if port := u.Port(); port != "" {
		n, err := strconv.ParseUint(port, 10, 16)
		if err != nil || n == 0 {
			return "", invalid
		}
	}
	if net.ParseIP(host) != nil {
		// IPv6 authorities require brackets, including when no port is present.
		if strings.Contains(host, ":") && !strings.HasPrefix(authority, "[") {
			return "", invalid
		}
		return host, nil
	}
	if len(host) > 253 || strings.ContainsAny(authority, "[]") {
		return "", invalid
	}
	for _, label := range strings.Split(host, ".") {
		if len(label) == 0 || len(label) > 63 || strings.HasPrefix(label, "-") ||
			strings.HasSuffix(label, "-") || strings.Trim(label, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != "" {
			return "", invalid
		}
	}
	return host, nil
}

func envInt(key string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(key)); err == nil {
		return v
	}
	return def
}

func parseEvent(s string) (time.Time, error) {
	s = strings.TrimSpace(strings.Trim(s, `"`))
	if s == "" {
		return time.Time{}, nil
	}
	if n, err := strconv.ParseInt(s, 10, 64); err == nil {
		return time.Unix(n, 0), nil
	}
	t, err := time.Parse(time.RFC3339, s)
	if err != nil {
		return time.Time{}, fmt.Errorf("want RFC3339 with offset or a unix epoch: %w", err)
	}
	return t, nil
}

func logRequests(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.URL.Path, "/api/events") {
			log.Printf("%s %s %s", r.RemoteAddr, r.Method, r.URL.Path)
		}
		next.ServeHTTP(w, r)
	})
}
