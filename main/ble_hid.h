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
 * @brief 重置蓝牙配对：擦除 NVS 中的绑定密钥并重启，重启后回到未配对状态重新广播。
 * @note 调用后不会返回。
 */
void ble_hid_reset_bonding(void);

/**
 * @brief 发送带修饰键的击键（如 Cmd+Shift+Return）
 * @param modifier HID 修饰键位图（0x01=L-Ctrl, 0x02=L-Shift, 0x04=L-Alt, 0x08=L-Cmd/GUI...）
 * @param keycode  HID Usage ID
 */
void ble_hid_key_press_mod(uint8_t modifier, uint8_t keycode);

/**
 * @brief 发送"开始放映"组合击键，同时兼容 Windows 与 macOS：
 *        先发 F5（Windows/WPS/LibreOffice），再发 Cmd+Shift+Return（macOS PowerPoint）
 */
void ble_hid_press_start_slideshow(void);

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
