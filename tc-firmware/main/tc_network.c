/*
 * Created by rmukhia on 10/4/25.
 *************************************************************/

#include "tc_network.h"

#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_event.h>
#include <string.h>
#include <time.h>
#include <esp_netif_sntp.h>
#if CONFIG_TC_MQTT_ENABLED
#include <mqtt_client.h>
#else
#include <esp_http_client.h>
#endif

#include "utils.h"

static const char* TAG = "tc-network";

typedef enum wifi_status_e
{
    WIFI_STAT_UNINITD,
    WIFI_STAT_INITD,
    WIFI_STAT_CONNECTING,
    WIFI_STAT_CONNECTED,
} wifi_status_t;

typedef enum mqtt_status_e
{
    MQTT_STATE_UNINIT = 0,
    MQTT_STATE_INIT,
    MQTT_STATE_CONNECTED,
} mqtt_status_t;

static struct
{
    struct
    {
        volatile wifi_status_t status;

        struct
        {
            esp_netif_t* netif;
            wifi_config_t config;
            EventGroupHandle_t event_group;
        } sta;

        esp_event_handler_instance_t evt_wifi;
        esp_event_handler_instance_t evt_got_ip;
        char sta_ip[IP4ADDR_STRLEN_MAX];
        esp_timer_handle_t connect_timer;
        int connect_retries;
    } wifi;

#if CONFIG_TC_MQTT_ENABLED
    struct
    {
        volatile mqtt_status_t state;
        char mqtt_host[128];
        esp_mqtt_client_handle_t client;
    } mqtt;
#endif

    bool sntp_started;
    tc_network_established_cb_t established_cb;
} context =
{
    .wifi = {
        .status = WIFI_STAT_UNINITD,
        .sta = {
            .netif = NULL,
            .config = {
                .sta = {
                    .threshold.authmode = WIFI_AUTH_OPEN,
                    .channel = 0,
                    .password = "",
                }
            },
        },
        .sta_ip = {0},
        .connect_retries = 0,
        .connect_timer = NULL,
    },
#if TC_MQTT_ENABLED
    .mqtt = {
        .state = MQTT_STATE_UNINIT,
        .client = NULL,
        .mqtt_host = {0},
    },
#endif
    .sntp_started = false,
    .established_cb = NULL,
};

static void log_error_if_nonzero(const char* message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*********************************************
 * SNTP Related Functions
 *********************************************/

void _sntp_time_sync_notification_cb(struct timeval* tv)
{
    char temp_buf[64];
    struct tm now_time;
    now_time = *localtime(&tv->tv_sec);
    strftime(temp_buf, sizeof temp_buf, "%d-%m-%Y %H:%M:%S", &now_time);
    ESP_LOGI(TAG, "SNTP SYNC: %s.%06ld", temp_buf, tv->tv_usec);
}

static esp_err_t _sntp_start()
{
    if (!context.sntp_started)
    {
        ESP_LOGI(TAG, "SNTP started.");
        esp_sntp_config_t config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TC_SNTP_SERVER);
        config.sync_cb = _sntp_time_sync_notification_cb;
        esp_netif_sntp_init(&config);
        context.sntp_started = true;
    }
    return ESP_OK;
}

esp_err_t _sntp_stop()
{
    if (context.sntp_started)
    {
        esp_netif_sntp_deinit();
        context.sntp_started = false;
    }
    return ESP_OK;
}


#if CONFIG_TC_MQTT_ENABLED
/*********************************************
 * MQTT Related Functions
 *********************************************/

static void mqtt_event_handler(void* handler_args, esp_event_base_t base,
                               const int32_t event_id, void* event_data)
{
    ESP_LOGD(TAG,
             "Event dispatched from event loop base=%s, event_id=%" PRIi32 "",
             base, event_id);
    const esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        {
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            context.mqtt.state = MQTT_STATE_CONNECTED;
            // for mqtt the connection is established after mqtt connection is made.
            if (context.established_cb != NULL) context.established_cb();
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        context.mqtt.state = MQTT_STATE_INIT;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        {
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type ==
            MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls",
                                 event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack",
                                 event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero(
                "captured as transport's socket errno",
                event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(
                TAG, "Last errno string (%s)",
                strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}


static esp_mqtt_client_config_t __make_client_config()
{
    CLEAR_ARRAY(context.mqtt.mqtt_host);

    snprintf(context.mqtt.mqtt_host, 128, "%s", CONFIG_TC_MQTT_BROKER_URL);

    ESP_LOGI(TAG, "connection url: %s", context.mqtt.mqtt_host);


    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = context.mqtt.mqtt_host
    };

    return mqtt_cfg;
}

static esp_err_t _mqtt_init()
{
    if (context.mqtt.state != MQTT_STATE_UNINIT)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mqtt_client_config_t mqtt_cfg = __make_client_config();

    context.mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    configASSERT(context.mqtt.client);

    VERIFY_SUCCESS(esp_mqtt_client_register_event(
        context.mqtt.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));


    context.mqtt.state = MQTT_STATE_INIT;
    return ESP_OK;
}


static esp_err_t _mqtt_connect()
{
    if (context.mqtt.state != MQTT_STATE_INIT)
    {
        return ESP_ERR_INVALID_STATE;
    }


    return esp_mqtt_client_start(context.mqtt.client);
}


esp_err_t tc_mqtt_publish_telemetry(const char* topic, const char* data,
                                    const size_t data_len)
{
    if (context.mqtt.state != MQTT_STATE_CONNECTED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending MQTT message to topic: %s %s", topic, data);

    esp_mqtt_client_publish(context.mqtt.client, topic, data,
                            (int)data_len, 0, 0);

    return ESP_OK;
}

#else
/*********************************************
 * HTTP Related Functions
 *********************************************/

esp_err_t tc_http_publish_telemetry(const char* data,
                                    const size_t data_len)
{
    if (context.wifi.status != WIFI_STAT_CONNECTED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_http_client_config_t config = {
        .url = CONFIG_TC_HTTP_SERVER_URL,
        .method = HTTP_METHOD_POST,
    };

    const esp_http_client_handle_t client = esp_http_client_init(&config);
    VERIFY_SUCCESS(esp_http_client_set_post_field(client, data, data_len));

    ESP_LOGI(TAG, "Sending HTTP message to url: %s %s", config.url, data);

    VERIFY_SUCCESS(esp_http_client_perform(client));
    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %llu",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));

    return esp_http_client_cleanup(client);
}
#endif


/*********************************************
 * Wi-Fi Related Functions
 *********************************************/

static uint64_t __wifi_get_next_connect()
{
    // double the time,in seconds
    return context.wifi.connect_retries * 2 * 1000000;
}

static void __wifi_connect_timer(__attribute__((unused)) void* args)
{
    esp_wifi_connect();
}

static void __wifi_event_sta_handler(int32_t event_id, void* event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        {
            esp_timer_start_once(context.wifi.connect_timer,
                                 __wifi_get_next_connect());
            context.wifi.status = WIFI_STAT_CONNECTING;
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t* event =
                (wifi_event_sta_disconnected_t*)event_data;
            ESP_LOGI(TAG, "connect sta to %s : %s failed. reason %d. retry %d",
                     event->ssid, event->bssid, event->reason,
                     context.wifi.connect_retries);
            CLEAR_ARRAY(context.wifi.sta_ip);
            context.wifi.connect_retries++;
            esp_timer_start_once(context.wifi.connect_timer,
                                 __wifi_get_next_connect());
            context.wifi.status = WIFI_STAT_CONNECTING;
        }
        break;

    case IP_EVENT_STA_GOT_IP:
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
            ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            snprintf(context.wifi.sta_ip, IP4ADDR_STRLEN_MAX, IPSTR,
                     IP2STR(&event->ip_info.ip));

            context.wifi.connect_retries = 0;
            context.wifi.status = WIFI_STAT_CONNECTED;
            // start sntp to get time
            ESP_ERROR_CHECK_WITHOUT_ABORT(_sntp_start());

#if CONFIG_TC_MQTT_ENABLED
            // start mqtt connection
            ESP_ERROR_CHECK_WITHOUT_ABORT(_mqtt_connect());
#else
            // For http the connection is established
            if (context.established_cb != NULL) context.established_cb();
#endif
        }
        break;
    }
}


static void
_wifi_event_handler(__attribute__((unused)) void* arg,
                    __attribute__((unused)) esp_event_base_t event_base,
                    int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "event %ld", event_id);

    // station mode
    if (event_id == WIFI_EVENT_STA_START ||
        event_id == WIFI_EVENT_STA_DISCONNECTED ||
        event_id == IP_EVENT_STA_GOT_IP)
    {
        __wifi_event_sta_handler(event_id, event_data);
    }
}

esp_err_t tc_network_start(tc_network_established_cb_t cb)
{
    ESP_LOGI(TAG, "Status is %d", context.wifi.status);
    if (context.wifi.status != WIFI_STAT_UNINITD)
    {
        return ESP_ERR_INVALID_STATE;
    }

    context.established_cb = cb;

    VERIFY_SUCCESS(esp_netif_init());
    ESP_LOGI(TAG, "Status is %d", context.wifi.status);


    strcpy((char*)context.wifi.sta.config.sta.ssid, CONFIG_TC_WIFI_STA_SSID);


    if (strlen(CONFIG_TC_WIFI_STA_PASSWORD) == 0)
    {
        ESP_LOGI(TAG, "Connecting to open wifi!");
    }
    else
    {
        strcpy((char*)context.wifi.sta.config.sta.password, CONFIG_TC_WIFI_STA_PASSWORD);
    }

    ESP_LOGI(TAG, "Connecting to wifi %s %s", context.wifi.sta.config.sta.ssid, context.wifi.sta.config.sta.password);

    const wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    VERIFY_SUCCESS(esp_wifi_init(&cfg));

    VERIFY_SUCCESS(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    VERIFY_SUCCESS(esp_wifi_set_mode(WIFI_MODE_STA));

    VERIFY_SUCCESS(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL,
        &context.wifi.evt_wifi));

    context.wifi.sta.netif = esp_netif_create_default_wifi_sta();

    VERIFY_SUCCESS(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL,
        &context.wifi.evt_got_ip));
    esp_timer_create_args_t timer_args = {
        .callback = __wifi_connect_timer,
        .arg = &context.wifi.connect_retries,
    };
    VERIFY_SUCCESS(
        esp_timer_create(&timer_args, &context.wifi.connect_timer));
    VERIFY_SUCCESS(
        esp_wifi_set_config(WIFI_IF_STA, &context.wifi.sta.config));

    context.wifi.status = WIFI_STAT_INITD;

#if CONFIG_TC_MQTT_ENABLED
    // initialize mqtt
    _mqtt_init();
#endif

    VERIFY_SUCCESS(esp_wifi_start());

    return ESP_OK;
}
