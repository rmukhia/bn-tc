/*
 * Created by rmukhia on 11/4/25.
 *************************************************************/

#pragma once
#define CLEAR_STRUCT(x) memset(&x, 0, sizeof(x))
#define CLEAR_ARRAY(x)  memset(x, 0, sizeof(x))

#define VERIFY_SUCCESS(x)                                   \
do {                                                        \
    esp_err_t r = x;                                        \
    if (r != ESP_OK) {                                      \
        ESP_LOGI(TAG, "%d:%s", __LINE__, __FUNCTION__);     \
        return r;                                           \
    }                                                       \
} while (0)
