// main/ble_hid.h — BLE HID Keyboard 服务接口（模拟键盘控制 PPT 翻页）
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief 初始化 BLE HID Keyboard 服务
 * @return ESP_OK 成功
 */
esp_err_t ble_hid_init(void);

/**
 * @brief 发送按键按下+释放（单次击键）
 * @param keycode HID Usage ID（如 0x50 = Left Arrow）
 * @note 非阻塞，内部通过任务执行
 */
void ble_hid_key_press(uint8_t keycode);

/**
 * @brief 检查是否有设备连接
 * @return true 已连接
 */
bool ble_hid_is_connected(void);

/**
 * @brief 获取已连接设备的 MAC 地址字符串（用于 UI 显示）
 * @param buf 输出缓冲区
 * @param len 缓冲区长度（建议 >= 18）
 * @return true 已连接并写入，false 未连接
 */
bool ble_hid_get_peer_str(char *buf, size_t len);

/**
 * @brief 停止 BLE HID 服务
 */
void ble_hid_stop(void);
