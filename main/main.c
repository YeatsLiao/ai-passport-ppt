// main/main.c — AI Passport PPT Remote 主程序
//
// BLE HID Keyboard 遥控器，控制 PPT 翻页:
//   上键 短按 → Left Arrow  (上一页)
//   下键 短按 → Right Arrow (下一页)
//   确认键 短按 → 开始放映（F5 / macOS Cmd+Shift+Return / Keynote Opt+Cmd+P，自动兼容）
//   确认键 长按 → Escape    (退出放映 + 停止计时器)
//
// 兼容: PowerPoint / WPS / LibreOffice Impress / Keynote

#include "ble_hid.h"
#include "bsp_battery.h"
#include "fap_screenshot.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "main";

// ================================================================
// HID 键盘码
// ================================================================
#define HID_KEY_LEFT_ARROW  0x50
#define HID_KEY_RIGHT_ARROW 0x4F
#define HID_KEY_ESCAPE      0x29

// ================================================================
// UI 对象
// ================================================================
static lv_obj_t *s_action_label;    // 当前操作反馈
static lv_obj_t *s_conn_label;      // 连接状态
static lv_obj_t *s_battery_label;   // 电量
static lv_obj_t *s_timer_label;     // 演讲计时器
static lv_timer_t *s_status_timer;  // 状态刷新定时器
static lv_timer_t *s_pres_timer;    // 演讲计时器定时器

// ================================================================
// 演讲计时器状态
// ================================================================
static bool s_pres_running = false;   // 是否正在演示
static uint32_t s_pres_seconds = 0;   // 累计秒数

// ================================================================
// 状态刷新定时器 (500ms, 已持有 LVGL 锁)
// ================================================================
static void status_tick(lv_timer_t *timer)
{
    (void)timer;

    // 电量刷新
    int soc = bsp_battery_soc();
    if (soc >= 0 && soc <= 100) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", soc);
        lv_label_set_text(s_battery_label, buf);
    } else {
        lv_label_set_text(s_battery_label, "--");
    }

    // 连接状态 (醒目显示, 参考 TikTok Remote)
    if (ble_hid_is_connected()) {
        char peer[20];
        char buf[48];
        if (ble_hid_get_peer_str(peer, sizeof(peer))) {
            snprintf(buf, sizeof(buf), "Connected\n%s", peer);
        } else {
            snprintf(buf, sizeof(buf), "Connected");
        }
        lv_label_set_text(s_conn_label, buf);
        lv_obj_set_style_text_color(s_conn_label, lv_color_hex(0x00FF00), 0);
    } else {
        lv_label_set_text(s_conn_label, "Pair with\nPPT-Remote");
        lv_obj_set_style_text_color(s_conn_label, lv_color_hex(0xFFAA00), 0);
    }
}

// ================================================================
// 演讲计时器 (1s 刷新, 已持有 LVGL 锁)
// ================================================================
static void pres_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_pres_running) return;

    s_pres_seconds++;

    uint32_t h = s_pres_seconds / 3600;
    uint32_t m = (s_pres_seconds % 3600) / 60;
    uint32_t s = s_pres_seconds % 60;

    char buf[16];
    if (h > 0) {
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(buf, sizeof(buf), "%02lu:%02lu",
                 (unsigned long)m, (unsigned long)s);
    }
    lv_label_set_text(s_timer_label, buf);
}

// 重置计时器显示
static void timer_reset_display(void)
{
    s_pres_seconds = 0;
    lv_label_set_text(s_timer_label, "00:00");
}

// ================================================================
// 按键回调 (运行在 button 组件的任务中)
// HID 操作通过队列异步执行，不阻塞按键回调
// ================================================================
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;

    switch (btn) {
    case BSP_BTN_UP:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "UP -> Left Arrow (prev slide)");
            ble_hid_key_press(HID_KEY_LEFT_ARROW);
            if (bsp_lvgl_lock(500)) {
                lv_label_set_text(s_action_label, "<< Prev");
                bsp_lvgl_unlock();
            }
        }
        break;

    case BSP_BTN_DOWN:
        if (ev == BSP_BTN_CLICK) {
            ESP_LOGI(TAG, "DOWN -> Right Arrow (next slide)");
            ble_hid_key_press(HID_KEY_RIGHT_ARROW);
            if (bsp_lvgl_lock(500)) {
                lv_label_set_text(s_action_label, "Next >>");
                bsp_lvgl_unlock();
            }
        }
        break;

    case BSP_BTN_OK:
        if (ev == BSP_BTN_CLICK) {
            // 短按: 开始放映（跨平台: Windows F5 / macOS PPT Cmd+Shift+Return / Keynote Opt+Cmd+P）
            ESP_LOGI(TAG, "OK -> start slideshow (cross-platform)");
            ble_hid_press_start_slideshow();

            // 计时器: 仅首次 F5 启动, 后续按不重复重置
            if (!s_pres_running) {
                s_pres_running = true;
                s_pres_seconds = 0;
                if (bsp_lvgl_lock(500)) {
                    timer_reset_display();
                    bsp_lvgl_unlock();
                }
                ESP_LOGI(TAG, "Timer started");
            }

            if (bsp_lvgl_lock(500)) {
                lv_label_set_text(s_action_label, "START");
                lv_obj_set_style_text_color(s_action_label, lv_color_hex(0x00FF00), 0);
                bsp_lvgl_unlock();
            }
        } else if (ev == BSP_BTN_LONG) {
            // 长按: Escape 退出放映 + 停止并重置计时器
            ESP_LOGI(TAG, "OK Long -> Escape (exit slideshow)");
            ble_hid_key_press(HID_KEY_ESCAPE);

            s_pres_running = false;

            if (bsp_lvgl_lock(500)) {
                lv_label_set_text(s_action_label, "EXIT");
                lv_obj_set_style_text_color(s_action_label, lv_color_hex(0xFF8800), 0);
                timer_reset_display();
                bsp_lvgl_unlock();
            }
            ESP_LOGI(TAG, "Timer stopped & reset");
        }
        break;
    }
}

// ================================================================
// 构建 UI (调用前已持有 LVGL 锁)
//
// 屏幕布局 (240x320):
// ┌──────────────────────┐
// │              [BAT 85%]│  电量 (右上角)
// │                      │
// │     PPT Remote       │  标题 (白色, 20号)
// │                      │
// │    ┌──────────────┐  │
// │    │  Connected   │  │  连接状态 (居中)
// │    └──────────────┘  │
// │                      │
// │      00:12:34        │  演讲计时器 (大号, 居中)
// │                      │
// │  UP:Prev  DOWN:Next  │  操作提示 (灰色)
// │  OK:Start LONG:Exit  │
// │                      │
// │      << Prev         │  操作反馈 (底部, 绿色)
// └──────────────────────┘
// ================================================================
static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    // 电量显示 (右上角)
    s_battery_label = lv_label_create(scr);
    lv_label_set_text(s_battery_label, "--");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0x00CCFF), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -8, 8);

    // 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "PPT Remote");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    // 连接状态 (醒目显示)
    s_conn_label = lv_label_create(scr);
    lv_label_set_text(s_conn_label, "Pair with\nPPT-Remote");
    lv_obj_set_style_text_color(s_conn_label, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(s_conn_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_conn_label, 200);
    lv_obj_set_style_text_align(s_conn_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_conn_label, LV_ALIGN_CENTER, 0, -50);

    // 演讲计时器
    s_timer_label = lv_label_create(scr);
    lv_label_set_text(s_timer_label, "00:00");
    lv_obj_set_style_text_color(s_timer_label, lv_color_hex(0x00FFAA), 0);
    lv_obj_set_style_text_font(s_timer_label, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_timer_label, 200);
    lv_obj_set_style_text_align(s_timer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_timer_label, LV_ALIGN_CENTER, 0, -10);

    // 操作提示
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "UP: Prev   DOWN: Next\nOK: Start   LONG: Exit");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_width(hint, 220);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    // 当前操作反馈
    s_action_label = lv_label_create(scr);
    lv_label_set_text(s_action_label, "Ready");
    lv_obj_set_style_text_color(s_action_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(s_action_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_action_label, 200);
    lv_obj_set_style_text_align(s_action_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_action_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    // 状态刷新定时器 (500ms)
    s_status_timer = lv_timer_create(status_tick, 500, NULL);

    // 演讲计时器 (1000ms)
    s_pres_timer = lv_timer_create(pres_tick, 1000, NULL);
}

// ================================================================
// 入口
// ================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport PPT Remote (HID Keyboard)");

    // 初始化 NVS (BLE 需要)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化外设
    bsp_i2c_init();
    bsp_i2c_scan();

    // 初始化电量计 (芯片不在位时忽略)
    if (bsp_battery_init() == ESP_OK) {
        ESP_LOGI(TAG, "Battery gauge ready");
    } else {
        ESP_LOGW(TAG, "Battery gauge not found");
    }

    // 初始化显示
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display init failed");
        return;
    }
    bsp_display_backlight(100);

    // 初始化按键
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Button init failed");
        return;
    }

    // 构建 UI
    if (bsp_lvgl_lock(1000)) {
        build_ui();
        bsp_lvgl_unlock();
    }

    // 启动串口截屏协议监听 (FAP_SCREENSHOT_V1, 发布素材采集用)
    fap_screenshot_start();

    // 初始化 BLE HID Keyboard
    esp_err_t err = ble_hid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID init failed: %d", err);
        if (bsp_lvgl_lock(1000)) {
            lv_label_set_text(s_conn_label, "BLE Init FAIL");
            lv_obj_set_style_text_color(s_conn_label, lv_color_hex(0xFF0000), 0);
            bsp_lvgl_unlock();
        }
        return;
    }

    ESP_LOGI(TAG, "Ready: pair 'PPT-Remote' in PC Bluetooth");
}
