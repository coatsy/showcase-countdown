package main

import (
	"net/http/httptest"
	"strings"
	"testing"
)

func TestRecentAfterResumesWithoutReplayingEvents(t *testing.T) {
	bus := NewBus(10)
	bus.Emit(Event{Kind: "error", Device: "bridge", Text: "first"})
	bus.Emit(Event{Kind: "presence", Device: "bridge", Text: "second"})

	recent := bus.Recent(10)
	resumed := bus.RecentAfter(recent[0].id, 10)
	if len(resumed) != 1 || resumed[0].Text != "second" {
		t.Fatalf("unexpected resumed events: %+v", resumed)
	}
	if replayed := bus.RecentAfter(recent[1].id, 10); len(replayed) != 0 {
		t.Fatalf("last event was replayed: %+v", replayed)
	}
}

func TestWriteSSEIncludesEventID(t *testing.T) {
	response := httptest.NewRecorder()
	writeSSE(response, Event{id: 42, Kind: "presence", Text: "online"})
	if !strings.HasPrefix(response.Body.String(), "id: 42\ndata: ") {
		t.Fatalf("SSE event has no resumable id: %q", response.Body.String())
	}
}
