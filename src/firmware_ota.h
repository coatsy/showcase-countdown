#pragma once

namespace firmware_ota {

bool enabled();
bool ready();
void poll(const char* deviceId, bool allowed, void (*onStart)());

}