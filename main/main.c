#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"      
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "mqtt_client.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#define WIFI_SSID       "Bin"
#define WIFI_PASS       "Khaitran2004"
#define MAX_RETRY_WIFI       5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define MQTT_BROKER_URI "mqtt://3.27.197.226:1883"
#define MQTT_TOPIC_SOIL_MOISTURE "/sensor/soil_moisture"

#define UART_NUM UART_NUM_2 //  UART2
#define TXD_PIN (GPIO_NUM_17) // Chân TX của UART1
#define RXD_PIN (GPIO_NUM_16) // Chân RX của UART1
#define BUF_SIZE (512)

static EventGroupHandle_t wifi_event_group;
static const char *TAG = "MQTT_UART";
static int retry_wifi = 0;

esp_mqtt_client_handle_t client;
uint8_t uart_rec_data[50];
uint8_t uart_trans_data[50];
int length;


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    client = event->client;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        
        esp_mqtt_client_subscribe(client, MQTT_TOPIC_SOIL_MOISTURE, 0);

        ESP_LOGI(TAG, "Subscribed to topics");
        break;

    case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED");
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED");
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED");
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);

        // So sánh topic nhận được với topic "/sensor/soil_moisture"
        if (strncmp(event->topic, MQTT_TOPIC_SOIL_MOISTURE, event->topic_len) == 0 && strlen(MQTT_TOPIC_SOIL_MOISTURE) == event->topic_len) 
        {
                // Sao chép dữ liệu nhận được vào uart_trans_data, đảm bảo null-terminated
            size_t len = event->data_len;
            if (len >= sizeof(uart_trans_data)) 
                {
                    len = sizeof(uart_trans_data) - 1;  // tránh tràn buffer
                }
            memcpy(uart_trans_data, event->data, len);
            uart_trans_data[len] = '\0'; // kết thúc chuỗi
        }
        break;

    default:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        ESP_LOGI(TAG, "[STA_START] esp_wifi_connect() = %s", esp_err_to_name(err));
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected reason: %d", disconn->reason);  // <- Thêm dòng này

        if (retry_wifi < MAX_RETRY_WIFI) {
        retry_wifi++;
        ESP_LOGW(TAG, "[DISCONNECTED] retry #%d", retry_wifi);
        esp_wifi_connect();
        } 
        else {
        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_wifi = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    EventBits_t bits = 0;
    wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA mode initialized");



    while (retry_wifi < MAX_RETRY_WIFI) {
        bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to Wi-Fi");
            break; 
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGI(TAG, "Failed to connect to Wi-Fi");
            break;
        }
    }


    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to Wi-Fi after %d attempts", retry_wifi);
        return;
    }
    if (bits & WIFI_CONNECTED_BIT) {
        mqtt_app_start();
    }
}


void uart_init() {
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    // Cấu hình UART1
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
}

void uart_read_task(void *arg) {
    uint8_t uart_data[50];
    while (1) {
        // Đọc dữ liệu từ UART với timeout 50ms
        int length = uart_read_bytes(UART_NUM, uart_data, 50 - 1, 50 / portTICK_PERIOD_MS);
        if (length > 0) {
            uart_data[length] = '\0';  // Kết thúc chuỗi an toàn
            
            ESP_LOGI(TAG, "UART received: %s", (char *)uart_data);

            // Copy dữ liệu sang biến toàn cục để các task khác xử lý (nên dùng mutex nếu mở rộng)
            strncpy((char *)uart_rec_data, (char *)uart_data, 50 - 1);
            uart_rec_data[50 - 1] = '\0'; // đảm bảo null-terminated
        }

        vTaskDelay(500 / portTICK_PERIOD_MS); // delay ngắn hơn tránh tốn CPU
    }
}


void mqtt_publish_task(void *pvParameters) {
    while (1) {
        if (strstr((char *)uart_rec_data, "\"sensor\":\"Soil Moisture Sensor\"") != NULL) {
            esp_mqtt_client_publish(client, MQTT_TOPIC_SOIL_MOISTURE, (char *)uart_rec_data, strlen((char *)uart_rec_data), 1, 0);
            memset(uart_rec_data, 0, sizeof(uart_rec_data));
        }
        vTaskDelay(500/portTICK_PERIOD_MS);
    }
}


void uart_send_task(void *arg) {
    while (1) {
        if (uart_trans_data[0] != '\0') {  // nếu có dữ liệu
            uart_write_bytes(UART_NUM, (const char *)uart_trans_data, strlen((const char *)uart_trans_data));
            uart_write_bytes(UART_NUM, "\r\n", 2);  // thêm xuống dòng cho rõ ràng

            ESP_LOGI(TAG, "Sent via UART: %s", uart_trans_data);

        }

        vTaskDelay(500 / portTICK_PERIOD_MS); // gửi mỗi 500ms
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());


    ESP_LOGI(TAG, "Starting Wi-Fi Station");
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    wifi_init_sta();

    uart_init();
    vTaskDelay(1000);
    xTaskCreate(uart_read_task, "uart_read_task", 4096, NULL, 10, NULL);
    xTaskCreate(mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5, NULL);
    xTaskCreate(uart_send_task, "uart_send_task", 4096, NULL, 6, NULL);

    while (1)
    {
        /* code */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
}
