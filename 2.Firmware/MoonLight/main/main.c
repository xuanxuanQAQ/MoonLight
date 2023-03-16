#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "mqtt5_client.h"

#include "led_strip_types.h"
#include "led_strip_rmt.h"
#include "led_strip.h"

#define LED_GPIO_NUM 13                                 // LED灯条输出GPIO口
#define LED_NUM 10                                      // LED灯条LED灯数
#define RMT_RESOLUTION 10000000                         // RMT分辨率
#define WIFI_SSID "CMCC-9XAK"                           // wifi账号
#define WIFI_PASS "2Z9CKKLS"                            // wifi密码
#define WIFI_MAX_RETRY_NUM 5                            // wifi最大尝试重连次数
#define MQTT_BROKER_URL "mqtt://emqx@120.26.40.90:1883" // MQTT服务器地址

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#define USE_PROPERTY_ARR_SIZE sizeof(user_property_arr) / sizeof(esp_mqtt5_user_property_item_t)

static esp_mqtt5_user_property_item_t user_property_arr[] = {
    {"board", "esp32"},
    {"u", "user"},
    {"p", "password"}};

static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
    .response_topic = "/topic/test/response",
    .correlation_data = "123456",
    .correlation_data_len = 6,
};

static esp_mqtt5_subscribe_property_config_t subscribe_property = {
    .subscribe_id = 25555,
    .no_local_flag = false,
    .retain_as_published_flag = false,
    .retain_handle = 0,
    .is_share_subscribe = true,
    .share_name = "group1",
};

static esp_mqtt5_subscribe_property_config_t subscribe1_property = {
    .subscribe_id = 25555,
    .no_local_flag = true,
    .retain_as_published_flag = false,
    .retain_handle = 0,
};

static esp_mqtt5_unsubscribe_property_config_t unsubscribe_property = {
    .is_share_subscribe = true,
    .share_name = "group1",
};

static esp_mqtt5_disconnect_property_config_t disconnect_property = {
    .session_expiry_interval = 60,
    .disconnect_reason = 0,
};

static EventGroupHandle_t wifi_event_group_handle = NULL;
static uint32_t s_retry_num = 0; // wifi连接重试次数

void nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret == ESP_OK)
    {
        ESP_LOGI("NVS_INFO", "NVS初始化完成");
    }
    else
    {
        ESP_ERROR_CHECK(ret);
    }
}

/**
 * @brief HSV模型
 * @param[in] h-输入想要的颜色的色相（hue），范围[0，360]
 * @param[in] s-输入想要的颜色的饱和度（saturation），范围[0，100]
 * @param[in] v-输入想要的颜色的明度（value），范围[0，100]
 * @param[out] r,g,b-输出想要的颜色的rgb值
 *
 */
void led_strip_hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b)
{
    h %= 360;
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i)
    {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

/**
 * @brief LED灯条初始化函数
 * @param[out] 输出所生成的LED灯条对象的句柄
 *
 */
led_strip_handle_t LED_strip_init(void)
{
    led_strip_handle_t led_handle = NULL;
    led_strip_config_t led_config = {
        .strip_gpio_num = LED_GPIO_NUM,
        .max_leds = LED_NUM,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t led_rmt_config = {
        .clk_src = RMT_CLK_SRC_APB,
        .resolution_hz = RMT_RESOLUTION,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_config, &led_rmt_config, &led_handle));
    return led_handle;
}

/**
 * @brief 灯光颜色初始化函数
 *
 */
void LED_color_init(uint32_t *h, uint32_t *s, uint32_t *v)
{
    for (int i = 0; i < LED_NUM; i++)
    {
        h[i] = 28;
        s[i] = 100;
        v[i] = 10;
    }
}

/**
 * @brief 自定义灯光变化函数：此处为月球灯闪烁
 *
 * @param[in] h,s,v
 */
void LED_color_input(uint32_t *h, uint32_t *s, uint32_t *v)
{
    static uint32_t count = 0;
    static bool flag = true; // flag为true为增加，flag为false为减小
    count++;
    for (int i = 0; i < LED_NUM; i++)
    {
        if (flag)
        {

            v[i] += 2;
        }
        else
        {
            v[i] -= 2;
        }
    }
    if (count == 30)
    {
        flag = !flag;
        count = 0;
    }
}

/**
 * @brief LED灯条运行线程
 * @param[in] 输入灯条句柄
 *
 */
void LED_strip_run(void *led_handle_ptr)
{

    uint32_t red[LED_NUM] = {0};
    uint32_t green[LED_NUM] = {0};
    uint32_t blue[LED_NUM] = {0};
    uint32_t hue[LED_NUM] = {0};
    uint32_t saturation[LED_NUM] = {0};
    uint32_t value[LED_NUM] = {0};

    led_strip_handle_t led_handle = *(led_strip_handle_t *)led_handle_ptr;

    LED_color_init(hue, saturation, value);
    ESP_LOGI("LED_INFO", "ESP灯条颜色初始化完成");

    while (1)
    {
        LED_color_input(hue, saturation, value);
        // 为LED灯条的每个LED写入RGB值
        for (int i = 0; i < LED_NUM; i++)
        {
            led_strip_hsv2rgb(hue[i], saturation[i], value[i], &red[i], &green[i], &blue[i]);
            led_strip_set_pixel(led_handle, i, red[i], green[i], blue[i]);
        }
        // 刷新LED灯条
        led_strip_refresh(led_handle);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/**
 * @brief wifi事件监听函数
 *
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < WIFI_MAX_RETRY_NUM)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI("WIFI_INFO", "尝试重新连接到wifi");
        }
        else
        {
            xEventGroupSetBits(wifi_event_group_handle, WIFI_FAIL_BIT);
        }
        ESP_LOGI("WIFI_INFO", "连接到wifi失败");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI_INFO", "ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group_handle, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief wifi初始化函数
 *
 */
void wifi_init(void)
{
    wifi_event_group_handle = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_LOGI("WIFI_INFO", "wifi初始化完成");

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 等待wifi连接
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group_handle,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    // wifi连接信息
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI("WIFI_INFO", "成功连接到wifi SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI("WIFI_INFO", "连接到wifi失败 SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASS);
    }
    else
    {
        ESP_LOGE("WIFI_INFO", "我也不知道发生了啥QAQ");
    }
}

/**
 * @brief 打印用户属性信息
 *
 */
static void print_user_property(mqtt5_user_property_handle_t user_property)
{
    if (user_property)
    {
        uint8_t count = esp_mqtt5_client_get_user_property_count(user_property);
        if (count)
        {
            esp_mqtt5_user_property_item_t *item = malloc(count * sizeof(esp_mqtt5_user_property_item_t));
            if (esp_mqtt5_client_get_user_property(user_property, item, &count) == ESP_OK)
            {
                for (int i = 0; i < count; i++)
                {
                    esp_mqtt5_user_property_item_t *t = &item[i];
                    ESP_LOGI("MQTT_INFO", "key is %s, value is %s", t->key, t->value);
                    free((char *)t->key);
                    free((char *)t->value);
                }
            }
            free(item);
        }
    }
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE("MQTT_INFO", "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief MQTT事件监听函数
 *
 *  这个函数被MQTT客户端事件循环所调用
 *
 * @param handler_args 回调函数传入参数(这里传了个NULL，就是传了个屁喵~)
 * @param base 基础事件类型(在这里永远穿回的是MQTT事件喵~)
 * @param event_id 事件的ID，用于标识事件类型
 * @param event_data 指向事件数据的指针, esp_mqtt_event_handle_t.
 */
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGI("MQTT_INFO", "事件循环分发了事件了喵~ base=%s, event_id=%ld", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    ESP_LOGI("MQTT_INFO", "空闲堆内存大小：%lu, 最小为： %lu", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI("MQTT_INFO", "MQTT_EVENT_CONNECTED");
        print_user_property(event->property->user_property);
        esp_mqtt5_client_set_user_property(&publish_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
        esp_mqtt5_client_set_publish_property(client, &publish_property);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 1);
        esp_mqtt5_client_delete_user_property(publish_property.user_property);
        publish_property.user_property = NULL;
        ESP_LOGI("MQTT_INFO", "sent publish successful, msg_id=%d", msg_id);

        esp_mqtt5_client_set_user_property(&subscribe_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
        esp_mqtt5_client_set_subscribe_property(client, &subscribe_property);
        msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
        esp_mqtt5_client_delete_user_property(subscribe_property.user_property);
        subscribe_property.user_property = NULL;
        ESP_LOGI("MQTT_INFO", "sent subscribe successful, msg_id=%d", msg_id);

        esp_mqtt5_client_set_user_property(&subscribe1_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
        esp_mqtt5_client_set_subscribe_property(client, &subscribe1_property);
        msg_id = esp_mqtt_client_subscribe(client, "/topic/test", 0);
        esp_mqtt5_client_delete_user_property(subscribe1_property.user_property);
        subscribe1_property.user_property = NULL;
        ESP_LOGI("MQTT_INFO", "sent subscribe successful, msg_id=%d", msg_id);

        // esp_mqtt5_client_set_user_property(&unsubscribe_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
        // esp_mqtt5_client_set_unsubscribe_property(client, &unsubscribe_property);
        // msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos0");
        // ESP_LOGI("MQTT_INFO", "sent unsubscribe successful, msg_id=%d", msg_id);
        // esp_mqtt5_client_delete_user_property(unsubscribe_property.user_property);
        // unsubscribe_property.user_property = NULL;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI("MQTT_INFO", "MQTT_EVENT_DISCONNECTED");
        print_user_property(event->property->user_property);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI("MQTT_INFO", "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        esp_mqtt5_client_set_publish_property(client, &publish_property);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI("MQTT_INFO", "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI("MQTT_INFO", "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        esp_mqtt5_client_set_user_property(&disconnect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
        esp_mqtt5_client_set_disconnect_property(client, &disconnect_property);
        esp_mqtt5_client_delete_user_property(disconnect_property.user_property);
        disconnect_property.user_property = NULL;
        esp_mqtt_client_disconnect(client);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI("MQTT_INFO", "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        print_user_property(event->property->user_property);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI("MQTT_INFO", "MQTT_EVENT_DATA");
        print_user_property(event->property->user_property);
        ESP_LOGI("MQTT_INFO", "payload_format_indicator is %d", event->property->payload_format_indicator);
        ESP_LOGI("MQTT_INFO", "response_topic is %.*s", event->property->response_topic_len, event->property->response_topic);
        ESP_LOGI("MQTT_INFO", "correlation_data is %.*s", event->property->correlation_data_len, event->property->correlation_data);
        ESP_LOGI("MQTT_INFO", "content_type is %.*s", event->property->content_type_len, event->property->content_type);
        ESP_LOGI("MQTT_INFO", "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI("MQTT_INFO", "DATA=%.*s", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI("MQTT_INFO", "MQTT_EVENT_ERROR");
        print_user_property(event->property->user_property);
        ESP_LOGI("MQTT_INFO", "MQTT5 return code is %d", event->error_handle->connect_return_code);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI("MQTT_INFO", "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI("MQTT_INFO", "Other event id:%d", event->event_id);
        break;
    }
}

void mqtt_init(void)
{
    // 配置项
    esp_mqtt5_connection_property_config_t connect_property = {
        .session_expiry_interval = 10,
        .maximum_packet_size = 1024,
        .receive_maximum = 65535,
        .topic_alias_maximum = 2,
        .request_resp_info = true,
        .request_problem_info = true,
        .will_delay_interval = 10,
        .payload_format_indicator = true,
        .message_expiry_interval = 10,
        .response_topic = "/test/response",
        .correlation_data = "123456",
        .correlation_data_len = 6,
    };

    esp_mqtt_client_config_t mqtt5_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .network.disable_auto_reconnect = true,
        .credentials.username = NULL,
        .credentials.authentication.password = NULL,
        .session.last_will.topic = "/topic/will",
        .session.last_will.msg = "i will leave",
        .session.last_will.msg_len = 12,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };

    // MQTT客户端初始化
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt5_cfg);

    // 设置连接属性和用户属性
    esp_mqtt5_client_set_user_property(&connect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_user_property(&connect_property.will_user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_connect_property(client, &connect_property);

    // 删除数据，回收内存
    esp_mqtt5_client_delete_user_property(connect_property.user_property);
    esp_mqtt5_client_delete_user_property(connect_property.will_user_property);

    // 监听事件，连接至服务器
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    // 初始化NVS
    nvs_init();

    // LED初始化
    led_strip_handle_t led_handle = LED_strip_init();
    ESP_LOGI("LED_INFO", "灯条驱动安装成功");

    // 创建亮灯任务线程
    TaskHandle_t LED_strip_run_handle = NULL;
    xTaskCreate(LED_strip_run, "LED_strip_run", 1024 * 8, (void *)&led_handle, 2, &LED_strip_run_handle);

    // wifi初始化
    wifi_init();

    // mqtt初始化
    mqtt_init();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
