/*
 * Created by rmukhia on 10/4/25.
 *************************************************************/

#pragma once
#include <esp_err.h>

esp_err_t tc_wifi_start(void);

#if CONFIG_TC_MQTT_ENABLED
esp_err_t tc_mqtt_publish_telemetry(const char* topic, const char* data,
                                    const size_t data_len);
#else
esp_err_t tc_http_publish_telemetry(const char* data,
                                    const size_t data_len);
#endif
