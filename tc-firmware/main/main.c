#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include "tc_hal.h"
#include "tc_network.h"
#include "utils.h"


static const char* TAG = "tc-firmware";


typedef struct payload_s
{
    float latitude;
    float longitude;
    short battery_percentage;
    time_t timestamp;
} payload_t;

static uint64_t _encode_payload(const payload_t* payload)
{
    // payload is 5 bytes, which can be fit in uint64_t for easy encoding
    // [0x00, 0x00, 0x00, Lat Integral, Lat Fractional, Lng Integral, Lng Fractional, Battery Percentage Integral]
    uint64_t encoded_payload = 0;
    const uint8_t lat_integral = (uint8_t)payload->latitude;
    const uint8_t lat_fractional = (uint8_t)((payload->latitude - lat_integral) * 100);
    const uint8_t lng_integral = (uint8_t)payload->longitude;
    const uint8_t lng_fractional = (uint8_t)((payload->longitude - lng_integral) * 100);
    const uint8_t battery_percentage = (uint8_t)(payload->battery_percentage);

    encoded_payload = (((uint64_t)lat_integral << 32) |
        ((uint64_t)lat_fractional << 24) |
        ((uint64_t)lng_integral << 16) |
        ((uint64_t)lng_fractional << 8) |
        ((uint64_t)battery_percentage)) & 0xFFFFFFFFFF;

    return encoded_payload;
}

/*
 * Create JSON payload with device string, latitude, longitude, and battery percentage.
 * Device_str is 13 bytes (including null terminator).
 * Latitude is 4 bytes (float), Longitude is 4 bytes (float), Battery percentage is 2 bytes (short).
 * Timestamp is 4 bytes (uint32_t).
 */
static cJSON* _create_json_payload(const char* device_str, const payload_t* payload)
{
    char buffer[32];
    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return NULL;
    }

    cJSON_AddStringToObject(root, "id", device_str);

    const uint64_t encoded_payload = _encode_payload(payload);
    CLEAR_ARRAY(buffer);
    sprintf(buffer, "%010llX", encoded_payload);

    ESP_LOGI(TAG, "Payload encoded: %llu %s", encoded_payload, buffer);

    cJSON_AddStringToObject(root, "payload", buffer);

    struct tm tm_s;
    localtime_r(&payload->timestamp, &tm_s);

    CLEAR_ARRAY(buffer);
    sprintf(buffer, "%04d-%02d-%02d",
            tm_s.tm_year + 1900,
            tm_s.tm_mon + 1,
            tm_s.tm_mday);

    cJSON_AddStringToObject(root, "date", buffer);

    CLEAR_ARRAY(buffer);
    sprintf(buffer, "%02d:%02d:%02d",
            tm_s.tm_hour,
            tm_s.tm_min,
            tm_s.tm_sec);
    cJSON_AddStringToObject(root, "time", buffer);

    return root;
}


static char publish_topic[64];

static esp_err_t loop(const char* device_str)
{
    payload_t payload;
    VERIFY_SUCCESS(tc_get_gps_location(&payload.latitude, &payload.longitude));
    VERIFY_SUCCESS(tc_get_battery_percentage(&payload.battery_percentage));
    payload.timestamp = time(NULL);

    cJSON* json_payload = _create_json_payload(device_str, &payload);
    char* json_str = cJSON_PrintUnformatted(json_payload);

#if CONFIG_TC_MQTT_ENABLED
    tc_mqtt_publish_telemetry(publish_topic, json_str, strlen(json_str));
#else
    tc_http_publish_telemetry(json_str, strlen(json_str));
#endif

    free(json_str);
    cJSON_Delete(json_payload);

    return ESP_OK;
}

static esp_err_t _nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

_Noreturn void app_main(void)
{
    ESP_LOGI(TAG, "Starting Technical Challenge Firmware");

    ESP_ERROR_CHECK(_nvs_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char device_str[13];
    ESP_ERROR_CHECK(tc_get_device_str(device_str));
    ESP_LOGI(TAG, "Device String: %s", device_str);

    // prepare publish topic
    snprintf(publish_topic, 64, "tc-bn/telemetry/%s", device_str);

    ESP_ERROR_CHECK(tc_wifi_start());

    uint64_t interval_in_ms = CONFIG_TC_PAYLOAD_GPS_INTERVAL * 1000;

    TickType_t xLastWakeTime = xTaskGetTickCount();
    while (true)
    {

        const esp_err_t result = loop(device_str);

        if (result != ESP_OK)
        {
            ESP_LOGE(TAG, "Error in loop: %s", esp_err_to_name(result));
        }

        vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(interval_in_ms));
    }
}
