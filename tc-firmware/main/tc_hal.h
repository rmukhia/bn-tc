/*
 * Created by rmukhia on 11/6/25.
 *************************************************************/

#pragma once
#include <esp_err.h>


esp_err_t tc_get_device_str(char* device_str);
esp_err_t tc_get_gps_location(float* latitude, float* longitude);
esp_err_t tc_get_battery_percentage(short* battery_percentage);