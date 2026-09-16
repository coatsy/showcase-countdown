#pragma once

#include <esp_idf_version.h>
#include <mqtt_client.h>

#include "env_config.h"
#if MQTT_TLS
#include <esp_crt_bundle.h>
#endif

inline void configureMqttBroker(esp_mqtt_client_config_t& cfg) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    cfg.broker.address.uri = MQTT_URI;
    cfg.credentials.username = MQTT_USERNAME[0] ? MQTT_USERNAME : nullptr;
    cfg.credentials.authentication.password = MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr;
#if MQTT_TLS
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
#else
    cfg.uri = MQTT_URI;
    cfg.username = MQTT_USERNAME[0] ? MQTT_USERNAME : nullptr;
    cfg.password = MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr;
#if MQTT_TLS
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
#endif
}
