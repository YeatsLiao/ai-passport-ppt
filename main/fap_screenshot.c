// main/fap_screenshot.c — FAP_SCREENSHOT_V1 串口截屏协议实现 (分块渲染版)
//
// 供 FoloToy AI Passport 发布助手采集运行中的屏幕画面:
//   主机发送:  "FAP_SCREENSHOT_V1\n"
//   设备回复:  "FAP_SCREENSHOT_V1 <w> <h> RGB565LE <len>\n" + 二进制像素数据
//
// 使用分块渲染策略: 每次仅渲染 8 行 (3.75KB), 无需分配全屏 150KB 缓冲区。
// 载荷为 RGB565LE 小端、行主序。只读采集, 不改配置、不输出凭证。

#include "fap_screenshot.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/draw/snapshot/lv_snapshot.h"
#include "src/core/lv_refr_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/display/lv_display_private.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "fap_shot";

#define FAP_CMD    "FAP_SCREENSHOT_V1"
#define FAP_CMDLEN (sizeof(FAP_CMD) - 1)
#define CHUNK_ROWS 8  // 每次渲染行数 (8行 × 240 × 2 = 3840 字节)

// 完整写出, 失败时短暂重试
static void write_all(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    int retries = 0;
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            retries = 0;
        } else {
            if (++retries > 1000) return;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// 渲染单个 chunk 到 draw_buf (在 LVGL 锁内调用)
static bool render_chunk(lv_obj_t *obj, lv_draw_buf_t *draw_buf,
                         int32_t y_start, int32_t chunk_h, lv_color_format_t cf)
{
    int32_t w = draw_buf->header.w;

    lv_area_t chunk_area;
    chunk_area.x1 = 0;
    chunk_area.y1 = y_start;
    chunk_area.x2 = w - 1;
    chunk_area.y2 = y_start + chunk_h - 1;

    lv_obj_t *top_obj = lv_refr_get_top_obj(&chunk_area, obj);
    if (top_obj == NULL) {
        lv_draw_buf_clear(draw_buf, NULL);
        top_obj = obj;
    }

    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = draw_buf;
    layer.buf_area.x1 = 0;
    layer.buf_area.y1 = y_start;
    layer.buf_area.x2 = w - 1;
    layer.buf_area.y2 = y_start + chunk_h - 1;
    layer.color_format = cf;
    layer._clip_area = chunk_area;
    layer.phy_clip_area = chunk_area;

    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);

    lv_display_t *disp_old = lv_refr_get_disp_refreshing();
    lv_display_t *disp_new = lv_obj_get_display(obj);
    lv_layer_t *layer_old = disp_new->layer_head;
    disp_new->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp_new);

    if (top_obj == obj) {
        lv_obj_redraw(&layer, top_obj);
    } else {
        lv_obj_refr(&layer, top_obj);
    }

    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    disp_new->layer_head = layer_old;
    lv_refr_set_disp_refreshing(disp_old);

    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);

    return true;
}

static void send_screenshot(void)
{
    uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "capture request: largest_block=%u", (unsigned)largest_block);

    uint32_t w = BSP_LCD_W;
    uint32_t h = BSP_LCD_H;
    uint32_t row_bytes = w * 2;
    uint32_t payload = row_bytes * h;

    // 分配 chunk 缓冲区 (仅 ~3.75KB)
    lv_draw_buf_t *chunk_buf = lv_draw_buf_create(w, CHUNK_ROWS,
                                                   LV_COLOR_FORMAT_RGB565,
                                                   LV_STRIDE_AUTO);
    if (!chunk_buf) {
        ESP_LOGE(TAG, "chunk buffer alloc failed");
        return;
    }

    // 传输期间静默全部日志
    esp_log_level_set("*", ESP_LOG_NONE);

    // 发送协议头
    char header[64];
    int hlen = snprintf(header, sizeof(header),
                        FAP_CMD " %lu %lu RGB565LE %lu\n",
                        (unsigned long)w, (unsigned long)h, (unsigned long)payload);
    write_all(header, (size_t)hlen);

    // 分块渲染并发送
    for (uint32_t y = 0; y < h; y += CHUNK_ROWS) {
        uint32_t rows = (y + CHUNK_ROWS > h) ? (h - y) : CHUNK_ROWS;

        if (!bsp_lvgl_lock(200)) {
            esp_log_level_set("*", ESP_LOG_INFO);
            lv_draw_buf_destroy(chunk_buf);
            ESP_LOGW(TAG, "LVGL lock timeout at row %lu", (unsigned long)y);
            return;
        }

        render_chunk(lv_scr_act(), chunk_buf, y, rows, LV_COLOR_FORMAT_RGB565);
        bsp_lvgl_unlock();

        // 发送渲染数据 (处理 stride 对齐)
        const uint8_t *data = (const uint8_t *)chunk_buf->data;
        uint32_t stride = chunk_buf->header.stride;
        for (uint32_t r = 0; r < rows; r++) {
            write_all(data + (size_t)r * stride, row_bytes);
        }
    }

    esp_log_level_set("*", ESP_LOG_INFO);
    lv_draw_buf_destroy(chunk_buf);
    ESP_LOGI(TAG, "screenshot sent: %lux%lu (chunked, %u rows/chunk)",
             (unsigned long)w, (unsigned long)h, CHUNK_ROWS);
}

// 串口监听任务: 逐字节匹配命令
static void screenshot_task(void *arg)
{
    (void)arg;
    size_t idx = 0;

    while (1) {
        uint8_t c;
        int r = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(20));
        if (r != 1) continue;

        if ((char)c == FAP_CMD[idx]) {
            if (++idx == FAP_CMDLEN) {
                idx = 0;
                send_screenshot();
            }
        } else {
            idx = ((char)c == FAP_CMD[0]) ? 1 : 0;
        }
    }
}

void fap_screenshot_start(void)
{
    // 安装 USB-Serial-JTAG 驱动 (启用 RX)
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 512;
    cfg.tx_buffer_size = 512;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "usb_serial_jtag driver install failed");
    }

    xTaskCreate(screenshot_task, "fap_shot", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "FAP_SCREENSHOT_V1 listener ready (chunked mode)");
}
