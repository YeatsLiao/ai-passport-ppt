// main/ble_hid.c — BLE HID Keyboard（ESP-IDF esp_hid 官方组件实现）
//
// 实现方式：使用 ESP-IDF 官方 esp_hid 组件 + Bluedroid 协议栈，
// 1:1 参考官方示例 examples/bluetooth/esp_hid_device（IDF 5.5+）。
//
// 演进历史（详见 docs/development-log.md）：
//   v1 手写 NimBLE GATT   → Windows 不识别为键盘，失败
//   v2 旧版 esp_hidd_prf  → IDF 5.5 已移除该 API，编译失败
//   v3 esp_hid 官方组件   → 当前方案
//
// 组件自动构建完整 HID GATT 服务（Report Map/协议模式/加密权限），
// 只需调用 esp_hidd_dev_input_set 发送 8 字节键盘报告。

#include "ble_hid.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_hid_common.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_hid";

#define DEVICE_NAME             "PPT-Remote"
#define HID_KEYBOARD_REPORT_ID  1
#define HID_BATTERY_LEVEL       100

// ================================================================
// 键盘 Report Map (Report ID = 1)
// 输入报告 8 字节: [modifier][reserved][key0..key5]
// 输出报告 1 字节: LED 状态 (NumLock/CapsLock...)
// ================================================================
static const unsigned char s_keyboard_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, HID_KEYBOARD_REPORT_ID, //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0xE0,        //   Usage Minimum (0xE0)
    0x29, 0xE7,        //   Usage Maximum (0xE7)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data,Var,Abs)          ← modifier 字节
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Const)                 ← reserved 字节
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data,Var,Abs)         ← LED 报告
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Const)                ← LED 填充位
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Kbrd/Keypad)
    0x19, 0x00,        //   Usage Minimum (0x00)
    0x29, 0x65,        //   Usage Maximum (0x65)
    0x81, 0x00,        //   Input (Data,Array)            ← 6 个按键槽
    0xC0,              // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_keyboard_report_map,
        .len = sizeof(s_keyboard_report_map),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id          = 0x16C0,
    .product_id         = 0x05DF,
    .version            = 0x0100,
    .device_name        = DEVICE_NAME,
    .manufacturer_name  = "FoloToy",
    .serial_number      = "1234567890",
    .report_maps        = s_report_maps,
    .report_maps_len    = 1,
};

// ================================================================
// 状态
// ================================================================
static esp_hidd_dev_t *s_hid_dev = NULL;
static volatile bool s_connected = false;   // HID 层已连接
static volatile bool s_auth_ok = false;     // 配对认证完成（Windows 要求加密）
static esp_bd_addr_t s_peer_bda;
static volatile bool s_peer_valid = false;

// ================================================================
// BLE GAP 事件（配对认证 / 广播）
// ================================================================
static void ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (!param->ble_security.auth_cmpl.success) {
            // 认证失败 HID 将无法工作
            ESP_LOGE(TAG, "BLE AUTH ERROR: 0x%x", param->ble_security.auth_cmpl.fail_reason);
            s_auth_ok = false;
        } else {
            ESP_LOGI(TAG, "BLE AUTH SUCCESS");
            s_auth_ok = true;
            memcpy(s_peer_bda, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
            s_peer_valid = true;
        }
        break;

    case ESP_GAP_BLE_KEY_EVT:
        ESP_LOGI(TAG, "BLE GAP KEY type = %d", param->ble_security.ble_key.key_type);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        // 数字比较确认（Just Works 不会触发，保留以防对端要求）
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        // 接受对端发起的安全请求
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    default:
        break;
    }
}

// ================================================================
// 广播参数与加密配对配置（Just Works 绑定，无需输入 PIN）
// ================================================================
static esp_err_t ble_hid_adv_init(void)
{
    esp_err_t ret;

    // 广播中携带 HID Service UUID (0x1812, 128-bit 编码)
    const uint8_t hidd_service_uuid128[] = {
        0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
    };

    esp_ble_adv_data_t ble_adv_data = {
        .set_scan_rsp        = false,
        .include_name        = true,
        .include_txpower     = true,
        .min_interval        = 0x0006,
        .max_interval        = 0x0010,
        .appearance          = ESP_HID_APPEARANCE_KEYBOARD,
        .manufacturer_len    = 0,
        .p_manufacturer_data = NULL,
        .service_data_len    = 0,
        .p_service_data      = NULL,
        .service_uuid_len    = sizeof(hidd_service_uuid128),
        .p_service_uuid      = (uint8_t *)hidd_service_uuid128,
        .flag                = 0x6,
    };

    // 安全参数: Secure Connections + 绑定, Just Works（设备无输入输出能力）
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;

    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "set AUTHEN_REQ_MODE failed: %d", ret);
        return ret;
    }
    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "set IOCAP_MODE failed: %d", ret);
        return ret;
    }
    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "set INIT_KEY failed: %d", ret);
        return ret;
    }
    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "set RSP_KEY failed: %d", ret);
        return ret;
    }
    if ((ret = esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1)) != ESP_OK) {
        ESP_LOGE(TAG, "set MAX_KEY_SIZE failed: %d", ret);
        return ret;
    }

    if ((ret = esp_ble_gap_set_device_name(DEVICE_NAME)) != ESP_OK) {
        ESP_LOGE(TAG, "set_device_name failed: %d", ret);
        return ret;
    }
    if ((ret = esp_ble_gap_config_adv_data(&ble_adv_data)) != ESP_OK) {
        ESP_LOGE(TAG, "config_adv_data failed: %d", ret);
        return ret;
    }
    return ESP_OK;
}

static esp_err_t ble_hid_adv_start(void)
{
    static esp_ble_adv_params_t adv_params = {
        .adv_int_min        = 0x20,
        .adv_int_max        = 0x30,
        .adv_type           = ADV_TYPE_IND,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .channel_map        = ADV_CHNL_ALL,
        .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    return esp_ble_gap_start_advertising(&adv_params);
}

// ================================================================
// HID 设备事件
// ================================================================
static void hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID START -> adv start");
        ble_hid_adv_start();
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID CONNECT");
        s_connected = true;
        break;

    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "PROTOCOL MODE: %s", param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;

    case ESP_HIDD_OUTPUT_EVENT:
        // 主机写入的 LED 输出报告（NumLock 等），仅记录
        ESP_LOGI(TAG, "OUTPUT ID:%u Len:%u", param->output.report_id, param->output.length);
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "HID DISCONNECT: %s",
                 esp_hid_disconnect_reason_str(esp_hidd_dev_transport_get(param->disconnect.dev),
                                               param->disconnect.reason));
        s_connected = false;
        s_auth_ok = false;
        // 断线后重新广播，允许再次配对
        ble_hid_adv_start();
        break;

    default:
        break;
    }
}

// ================================================================
// 控制器 + Bluedroid 底层初始化
// ================================================================
static esp_err_t init_low_level(void)
{
    esp_err_t ret;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    // ESP32-C3 仅 BLE，释放经典蓝牙内存
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        ESP_LOGE(TAG, "controller_mem_release failed: %d", ret);
        return ret;
    }

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "controller_init failed: %d", ret);
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "controller_enable failed: %d", ret);
        return ret;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (ret) {
        ESP_LOGE(TAG, "bluedroid_init failed: %d", ret);
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "bluedroid_enable failed: %d", ret);
        return ret;
    }

    ret = esp_ble_gap_register_callback(ble_gap_event_handler);
    if (ret) {
        ESP_LOGE(TAG, "gap_register_callback failed: %d", ret);
        return ret;
    }
    return ESP_OK;
}

// ================================================================
// 对外接口
// ================================================================
esp_err_t ble_hid_init(void)
{
    esp_err_t ret = init_low_level();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ble_hid_adv_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // HID 组件需要挂接 GATTS 回调
    ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS register callback failed: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "init esp_hid BLE keyboard: %s", DEVICE_NAME);
    ret = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %d", ret);
        return ret;
    }

    esp_hidd_dev_battery_set(s_hid_dev, HID_BATTERY_LEVEL);
    return ESP_OK;
}

void ble_hid_key_press(uint8_t keycode)
{
    if (s_hid_dev == NULL || !s_connected || !s_auth_ok) {
        ESP_LOGW(TAG, "Key 0x%02X ignored: not connected/authenticated", keycode);
        return;
    }

    // 8 字节报告: [modifier][reserved][key0..key5]
    uint8_t report[8] = {0};
    report[2] = keycode;  // 按下

    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0, HID_KEYBOARD_REPORT_ID, report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input_set (press) failed: %d", err);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    memset(report, 0, sizeof(report));  // 释放
    err = esp_hidd_dev_input_set(s_hid_dev, 0, HID_KEYBOARD_REPORT_ID, report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input_set (release) failed: %d", err);
    }
}

bool ble_hid_is_connected(void)
{
    return s_connected && s_auth_ok;
}

bool ble_hid_get_peer_str(char *buf, size_t len)
{
    if (!s_connected || !s_peer_valid || buf == NULL || len < 18) {
        return false;
    }
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             s_peer_bda[0], s_peer_bda[1], s_peer_bda[2],
             s_peer_bda[3], s_peer_bda[4], s_peer_bda[5]);
    return true;
}

void ble_hid_stop(void)
{
    esp_ble_gap_stop_advertising();
}
