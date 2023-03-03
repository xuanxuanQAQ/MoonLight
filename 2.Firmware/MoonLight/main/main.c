#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip_types.h"
#include "led_strip_rmt.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_err.h"

#define LED_GPIO_NUM 13         // LED灯条输出GPIO口
#define LED_NUM 1               // LED灯条LED灯数
#define RMT_RESOLUTION 10000000 // RMT分辨率

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

void app_main(void)
{
    // LED初始化
    led_strip_handle_t led_handle = LED_strip_init();
    ESP_LOGI("LED_INFO", "灯条驱动安装成功");

    // 创建亮灯任务线程
    TaskHandle_t LED_strip_run_handle = NULL;
    xTaskCreate(LED_strip_run, "LED_strip_run", 1024 * 8, (void *)&led_handle, 2, &LED_strip_run_handle);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
