/*
 * Created by rmukhia on 11/6/25.
 *************************************************************/

#include "tc_hal.h"
#include <esp_err.h>
#include <esp_mac.h>

/*
 * Get Device String based on MAC address.
 * The format is "ESP32_xxxxxx\0", where xxxxxx is the last 6 bytes of MAC in hex.
 * device_str should be at least 13 bytes long.
 */

esp_err_t tc_get_device_str(char* device_str)
{
    uint8_t mac[6];

    esp_err_t result = ESP_OK;
    if ((result = esp_efuse_mac_get_default((uint8_t*)&mac)) != ESP_OK)
    {
        return result;
    }

    uint64_t device_id = 0;
    for (int i = 5; i >= 0; i--)
    {
        device_id |= ((uint64_t)mac[i]) << ((5 - i) * 8);
    }

    sprintf((char*)device_str, "ESP32_%06llX", device_id & 0xFFFFFF);

    return result;
}


static float generate_random_float(const float min, const float max) {
    const float scale = (float)rand() / RAND_MAX; // Random float between 0.0 and 1.0
    return min + scale * (max - min);
}

esp_err_t tc_get_gps_location(float* latitude, float* longitude)
{
    // For simulation purposes, return random coordinates.
    *latitude = generate_random_float(13.40f, 13.90f);
    *longitude = generate_random_float(100.20f, 101.0f);
    return ESP_OK; // in case of sensor failure, return ESP_FAIL
}

esp_err_t tc_get_battery_percentage(short* battery_percentage)
{
    // For simulation purposes, return random battery percentage.
    *battery_percentage = (short)generate_random_float(10, 100);
    return ESP_OK; // in case of sensor failure, return ESP_FAIL
}
