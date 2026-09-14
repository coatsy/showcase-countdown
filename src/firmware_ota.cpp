#include "firmware_ota.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <string.h>

#include "env_config.h"

#ifndef OTA_PASSWORD_HASH
#define OTA_PASSWORD_HASH ""
#endif

namespace firmware_ota {
namespace {

bool listening = false;
bool partitionErrorReported = false;

bool validPasswordHash() {
    const char* hash = OTA_PASSWORD_HASH;
    if (strlen(hash) != 32) {
        return false;
    }
    for (size_t index = 0; index < 32; ++index) {
        if (!((hash[index] >= '0' && hash[index] <= '9') ||
              (hash[index] >= 'a' && hash[index] <= 'f'))) {
            return false;
        }
    }
    return true;
}

}

bool enabled() {
    static const bool configured = validPasswordHash();
    return configured;
}

bool ready() {
    return listening && WiFi.status() == WL_CONNECTED;
}

void poll(const char* deviceId, bool allowed, void (*onStart)()) {
    if (!enabled()) {
        return;
    }
    if (!allowed || WiFi.status() != WL_CONNECTED) {
        if (listening) {
            ArduinoOTA.end();
            listening = false;
            Serial.println("  ota     : paused");
        }
        return;
    }
    if (!listening) {
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (next == nullptr || running == nullptr || next->address == running->address) {
            if (!partitionErrorReported) {
                Serial.println("  ota     : unavailable; install the two-slot partition table over USB");
                partitionErrorReported = true;
            }
            return;
        }
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "showcase-%s", deviceId);
        ArduinoOTA.setHostname(hostname);
        ArduinoOTA.setPasswordHash(OTA_PASSWORD_HASH);
        ArduinoOTA.setPort(3232);
        ArduinoOTA.onStart([onStart]() {
            if (ArduinoOTA.getCommand() != U_FLASH) {
                Update.abort();
                return;
            }
            Serial.println("  ota     : updating application");
            if (onStart != nullptr) {
                onStart();
            }
        });
        ArduinoOTA.onEnd([]() { Serial.println("  ota     : complete; rebooting"); });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("  ota     : failed (%u)\n", static_cast<unsigned>(error));
        });
        ArduinoOTA.begin();
        listening = true;
        Serial.printf("  ota     : ready at %s:3232 (%s.local)\n",
                      WiFi.localIP().toString().c_str(), hostname);
    }
    ArduinoOTA.handle();
}

}