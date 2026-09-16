#pragma once
#include "relay_core.h"

namespace espnow_relay {
bool enabled();
void begin(bool bridge);
void poll();
bool send(relay_core::Topic topic, const char* payload);
void setReceiver(relay_core::Result (*receiver)(relay_core::Topic, const char*));
void serialLine(const char* line);
const char* timeSource();
uint32_t timeSyncMs();
void setTimeSource(const char* source);
bool ready();
}
