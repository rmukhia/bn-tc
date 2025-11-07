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
#include <math.h>
#include <nvs_flash.h>

#include "byteswap.h"
#include "cc.h"
#include "tc_hal.h"
#include "tc_network.h"
#include "utils.h"


static const char* TAG = "tc-firmware";


typedef struct data_s
{
    float latitude;
    float longitude;
    short battery_percentage;
    time_t timestamp;
} data_t;


/* Considering WGS84 coordinate system used in GPS the latitude ranges from -90 to +90
 * and longitude ranges from -180 to +180.
 *
 * We will encode latitude in 16 bits by scaling the continuous range [-90, +90] into [0, 65535]
 *   lat_u16 = round( (latitude + 90) / 180 * 65535 )
 *
 * We will encode longitude in 16 bits by scaling the continuous range [-180, +180] into [0, 65535]
 *   lon_u16 = round( (longitude + 180) / 360 * 65535 )
 *
 * Battery percentage will be stored in 8 bits (0-100).
 *
 * Total payload size = 16 + 16 + 8 = 40 bits = 5 bytes.
 */
#pragma pack(1)
typedef union
{
    struct
    {
        uint16_t lat_be; // big-endian
        uint16_t lon_be; // big-endian
        uint8_t battery_percent; // 0..100
    } f;

    uint8_t raw[5]; // raw bytes to hex-encode
} payload_t;

static payload_t _encode_payload(const data_t* data)
{
    payload_t payload;
    CLEAR_STRUCT(payload);

    // scale to uint16
    const uint16_t lat_u16 = (uint16_t)lroundf(((data->latitude + 90.0f) / 180.0f) * 65535.0f);
    const uint16_t lon_u16 = (uint16_t)lroundf(((data->longitude + 180.0f) / 360.0f) * 65535.0f);

    // store big-endian
    payload.f.lat_be = htons(lat_u16);
    payload.f.lon_be = htons(lon_u16);
    payload.f.battery_percent = (uint8_t)data->battery_percentage;

    return payload;
}

/*
 * Create JSON payload with device string, latitude, longitude, and battery percentage.
 */
static cJSON* _create_json_payload(const char* device_str, const data_t* payload)
{
    char buffer[32];
    cJSON* root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return NULL;
    }

    cJSON_AddStringToObject(root, "id", device_str);

    const payload_t encoded_payload = _encode_payload(payload);
    CLEAR_ARRAY(buffer);
    // Hex encode 5 bytes: "%02X%02X%02X%02X%02X"
    snprintf(buffer, sizeof(buffer),
             "%02X%02X%02X%02X%02X",
             encoded_payload.raw[0], encoded_payload.raw[1],
             encoded_payload.raw[2], encoded_payload.raw[3],
             encoded_payload.raw[4]);

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


#if CONFIG_TC_MQTT_ENABLED
static char publish_topic[64];
#endif


static void _print_data(const data_t* data)
{
    ESP_LOGI(TAG, "Latitude: %.2f", data->latitude);
    ESP_LOGI(TAG, "Longitude: %.2f", data->longitude);
    ESP_LOGI(TAG, "Battery Percentage: %d%%", data->battery_percentage);
    struct tm tm_s;
    localtime_r(&data->timestamp, &tm_s);
    ESP_LOGI(TAG, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d",
             tm_s.tm_year + 1900,
             tm_s.tm_mon + 1,
             tm_s.tm_mday,
             tm_s.tm_hour,
             tm_s.tm_min,
             tm_s.tm_sec);
}

static esp_err_t loop(const char* device_str)
{
    data_t payload;
    VERIFY_SUCCESS(tc_get_gps_location(&payload.latitude, &payload.longitude));
    VERIFY_SUCCESS(tc_get_battery_percentage(&payload.battery_percentage));
    payload.timestamp = time(NULL);

    _print_data(&payload);
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


static TaskHandle_t task_to_notify = NULL;

void network_established_cb(void)
{
    ESP_LOGI(TAG, "Network established callback called.");
    xTaskNotifyGive(task_to_notify);
}


void app_main(void)
{
    ESP_LOGI(TAG, "Starting Technical Challenge Firmware");

    ESP_ERROR_CHECK(_nvs_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char device_str[13];
    ESP_ERROR_CHECK(tc_get_device_str(device_str));
    ESP_LOGI(TAG, "Device String: %s", device_str);

#if CONFIG_TC_MQTT_ENABLED
    // prepare publish topic
    snprintf(publish_topic, 64, "tc-bn/telemetry/%s", device_str);
#endif

    task_to_notify = xTaskGetCurrentTaskHandle();

    ESP_ERROR_CHECK(tc_network_start(network_established_cb));

    // wait for network established callback, if no network for 5 minutes, reboot.
    const uint32_t notification_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300 * 1000));

    if (notification_value == 1)
    {
        const uint64_t interval_in_ms = CONFIG_TC_PAYLOAD_GPS_INTERVAL * 1000;

        TickType_t xLastWakeTime = xTaskGetTickCount();
        while (true)
        {
            const esp_err_t result = loop(device_str);

            if (result != ESP_OK)
            {
                ESP_LOGE(TAG, "Error in loop: %s", esp_err_to_name(result));
            }

            vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(interval_in_ms));
        }
    }
    else
    {
        ESP_LOGE(TAG, "Network timeout");
        esp_restart();
    }
}
