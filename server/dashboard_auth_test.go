package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestDashboardPassword(t *testing.T) {
	t.Setenv("DASHBOARD_PASSWORD", "test-password")
	app := &App{fleet: NewFleet(NewBus(10)), policy: NewPolicy(time.Time{})}
	mux := http.NewServeMux()
	mux.HandleFunc("/mcp", func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) })
	mux.HandleFunc("/mcp/{code}", func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(204) })
	app.dashboardRoutes(mux)
	for _, path := range []string{"/", "/index.html", "/api/fleet", "/api/events", "/api/send", "/api/organiser/lock", "/api/fake/fake01/button"} {
		for _, password := range []string{"", "wrong"} {
			req := httptest.NewRequest("GET", path, nil)
			if password != "" {
				req.SetBasicAuth("showcase", password)
			}
			w := httptest.NewRecorder()
			mux.ServeHTTP(w, req)
			if w.Code != 401 || w.Header().Get("WWW-Authenticate") == "" {
				t.Fatalf("%s: expected authentication challenge, got %d", path, w.Code)
			}
		}
	}
	for _, path := range []string{"/", "/api/fleet", "/mcp", "/mcp/8734"} {
		req := httptest.NewRequest("GET", path, nil)
		if path == "/" || path == "/api/fleet" {
			req.SetBasicAuth("showcase", "test-password")
		}
		w := httptest.NewRecorder()
		mux.ServeHTTP(w, req)
		if w.Code != 200 && w.Code != 204 {
			t.Fatalf("%s: expected access, got %d", path, w.Code)
		}
	}
	for _, origin := range []string{"https://attacker.example", "null"} {
		req := httptest.NewRequest("POST", "/api/fake/fake01/button", nil)
		req.SetBasicAuth("showcase", "test-password")
		req.Header.Set("Origin", origin)
		w := httptest.NewRecorder()
		mux.ServeHTTP(w, req)
		if w.Code != 403 {
			t.Fatalf("cross-origin action: got %d", w.Code)
		}
	}
	t.Setenv("DASHBOARD_PASSWORD", "")
	open := http.NewServeMux()
	app.dashboardRoutes(open)
	w := httptest.NewRecorder()
	open.ServeHTTP(w, httptest.NewRequest("GET", "/", nil))
	if w.Code != 200 {
		t.Fatalf("unconfigured LAN behavior changed: %d", w.Code)
	}
}
