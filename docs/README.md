# 技术文档

## 项目概述

AI Passport PPT Remote 是基于 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 硬件的 BLE 蓝牙 PPT 翻页遥控器。设备通过 BLE HID **键盘协议（Keyboard）** 向电脑发送按键，实现 PPT 翻页与放映控制，同时在设备屏幕上显示演讲计时。

## 功能

| 物理按键 | 事件 | HID 键码 | PPT 效果 |
|---------|------|----------|----------|
| 上键 | 单击 | `←` Left Arrow | 上一页 |
| 下键 | 单击 | `→` Right Arrow | 下一页 |
| 确认键 | 单击 | `F5` | 进入幻灯片放映 |
| 确认键 | 长按 | `Esc` | 退出放映 |

设备屏幕同时显示：电量百分比、蓝牙连接状态、演讲计时器、当前操作反馈。

## 技术架构

```
┌─────────────────────────────────────────────┐
│                  main/                       │
│  main.c        UI + 按键分发 + 演讲计时器     │
│  ble_hid.h/c   Keyboard HID 服务             │
├─────────────────────────────────────────────┤
│               components/bsp/                │
│  bsp_button / bsp_display / bsp_i2c /        │
│  bsp_battery / bsp_audio                     │
├─────────────────────────────────────────────┤
│   ESP-IDF 5.x + Bluedroid(esp_hidd) + LVGL   │
└─────────────────────────────────────────────┘
```

### 硬件层 (BSP)

复用官方 `ai-passport` 仓库的 BSP 组件：

- **bsp_button**: 三按键输入（上/下/确认），支持单击/双击/长按事件
- **bsp_display**: ST7789P3 240×320 LCD 驱动 + LVGL 集成（`bsp_lvgl_lock/unlock`）
- **bsp_i2c**: I2C 总线管理
- **bsp_battery**: CW2017 电量计，读取电量百分比和电压
- **bsp_audio**: 音频输出（本应用未使用，保留备用）

### 通信层 (BLE HID Keyboard)

基于 Bluedroid 协议栈的**官方 HIDD profile**（`esp_hidd_prf_api.h`）：

- **设备角色**: Peripheral，设备名 `PPT-Remote`
- **实现方式**: `esp_hidd_profile_init()` 自动构建完整 GATT 结构（HID Service +
  Device Information + Battery），内部 Report Map 含鼠标/键盘/LED/Consumer 报告
- **发按键**: `esp_hidd_send_keyboard_value(conn_id, modifier, &key, 1)`
- **安全**: Just Works 绑定（`ESP_LE_AUTH_BOND` + `ESP_IO_CAP_NONE`），
  输入报告在配对认证完成后才允许发送（`ESP_GAP_BLE_AUTH_CMPL_EVT`）
- **广播外观码**: 0x03C1 (Keyboard)

> ❗ 历史教训：手写 NimBLE GATT 键盘在 Windows 上无法被识别（能连接但按键无效），
> 详见 [开发日志](development-log.md) 中的踩坑记录。

### 应用层：按键模拟

```
按键回调 (button 任务)               hid_task (独立任务)
  ble_hid_key_press()   ──命令队列──>  执行按键序列:
  立即返回, 不阻塞                      按下 → 延时 50ms → 释放 → 延时 30ms
                                      esp_hidd_send_keyboard_value()
```

### 演讲计时器

- 按 F5（确认键短按）时自动启动，显示 `00:00` 开始计时
- 按 Esc（确认键长按）时自动停止
- 显示格式：`MM:SS`（超过 1 小时自动切换为 `HH:MM:SS`）
- 由 LVGL 1 秒定时器驱动，纯设备端实现

## 代码结构

```
├── main/
│   ├── main.c             # 主程序: NVS/外设初始化、UI、按键分发、计时器
│   ├── ble_hid.h          # HID 接口: key_press / is_connected / get_peer_str
│   ├── ble_hid.c          # Bluedroid esp_hidd Keyboard 服务 + 按键任务
│   ├── fap_screenshot.h/c # 串口截屏协议 (发布助手采集用)
│   └── CMakeLists.txt
├── components/bsp/        # 官方 AI Passport 硬件驱动
├── docs/
│   ├── README.md          # 本文档
│   └── development-log.md # 开发日志
├── CMakeLists.txt
├── sdkconfig.defaults
└── partitions.csv
```

## 环境安装（完整教程）

### 1. 安装 ESP-IDF

本项目需要 **ESP-IDF v5.x**（推荐 5.5+）。

#### Windows

1. 下载 [ESP-IDF Windows 安装包](https://dl.espressif.com/dl/esp-idf/)（离线安装器）
2. 运行安装器，选择 ESP-IDF 版本 ≥ 5.5，勾选 ESP32-C3 支持
3. 安装完成后，从开始菜单打开 **ESP-IDF CMD** 或 **ESP-IDF PowerShell** 终端
4. 验证环境：

```powershell
idf.py --version
```

> 如果输出版本号（如 `v5.5.3`），说明环境就绪。

#### Linux / macOS

```bash
# 安装依赖 (Ubuntu/Debian)
sudo apt install git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

# 克隆 ESP-IDF
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.3 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c3

# 激活环境（每次新开终端需执行）
. $HOME/esp/esp-idf/export.sh
idf.py --version
```

### 2. 获取项目代码

```bash
git clone https://github.com/YeatsLiao/ai-passport-ppt.git
cd ai-passport-ppt
```

### 3. 设置目标芯片

```bash
idf.py set-target esp32c3
```

> 此命令会根据 `sdkconfig.defaults` 生成 `sdkconfig`。如需修改配置，可运行 `idf.py menuconfig`。

### 4. 编译

```bash
idf.py build
```

首次编译会下载依赖组件（LVGL、Bluedroid 等），耗时较长。后续编译仅编译变更文件。

> 修改 `sdkconfig.defaults` 后需全量清理再构建：
> - **PowerShell**: `Remove-Item -Recurse -Force build; Remove-Item sdkconfig; idf.py build`
> - **CMD**: `rd /s /q build && del sdkconfig && idf.py build`
> - **Linux/macOS**: `rm -rf build sdkconfig && idf.py build`

### 5. 连接设备

1. 用 USB-C 数据线将 AI Passport 连接到电脑
2. 确认串口号：

**Windows**：打开 **设备管理器** → 端口(COM 和 LPT) → 找到 `USB Serial Device (COMx)`，记下 COM 号。

**Linux**：
```bash
ls /dev/ttyACM*
```

**macOS**：
```bash
ls /dev/cu.usbmodem*
```

### 6. 烧录固件

```bash
# 将 COM3 替换为你的实际串口号
idf.py -p COM3 flash monitor
```

或指定波特率加速烧录：

```bash
idf.py -p COM3 -b 460800 flash monitor
```

烧录完成后设备自动重启，屏幕显示 `PPT Remote` 界面。

> `monitor` 会同时打开串口监视器，按 `Ctrl+]` 退出。

### 7. 烧录预编译固件（可选）

从 [Releases](../../releases) 下载合并固件（命名 `ai-passport-ppt-full.bin`，`full` 表示从 `0x0` 整片烧录），使用 `esptool.py` 一步烧录：

```bash
esptool.py --chip esp32c3 -p COM3 --baud 460800 write_flash 0x0 ai-passport-ppt-full.bin
```

## 首次配对

### Windows 10/11

1. 烧录后设备自动开启蓝牙广播，屏幕显示 `Pairing...`（橙色）
2. 打开 **设置 → 蓝牙和其他设备**
3. 点击 **添加设备** → **蓝牙**
4. 搜索到 `PPT-Remote`，点击配对
5. 配对成功后设备屏幕显示 `Connected`（绿色）
6. 打开 PowerPoint/WPS，按键即可遥控

### macOS

1. 打开 **系统设置 → 蓝牙**
2. 在设备列表中找到 `PPT-Remote`，点击 **连接**
3. 连接成功后打开 Keynote/PowerPoint，按键即可遥控

### Linux (BlueZ)

```bash
# 使用 bluetoothctl 配对
bluetoothctl
> scan on
# 等待看到 PPT-Remote 的 MAC 地址
> pair XX:XX:XX:XX:XX:XX
> trust XX:XX:XX:XX:XX:XX
> connect XX:XX:XX:XX:XX:XX
> exit
```

> 更换固件版本或改动 HID 描述符后，需先在电脑蓝牙中**忽略/忘记旧设备**再重新配对。

## 故障排查

| 问题 | 可能原因 | 解决方法 |
|-----|---------|---------|
| 电脑搜不到设备 | 旧配对残留 / 未在广播 | 电脑忽略旧设备，重启遥控器 |
| 配对后按键无效 | 固件改动后描述符变化 | 忽略旧配对，重新配对 |
| 电量显示 `--` | 板上无 CW2017 电量计 | 正常现象，不影响其他功能 |
| F5 不触发放映 | PPT 软件未打开 / 焦点不在幻灯片区 | 确认 PPT 软件已打开且焦点在编辑区 |
| 计时器不走 | 未按确认键启动 | 短按确认键(F5)启动计时器 |
| 编译报 `idf_component.yml` 错误 | 未联网或代理问题 | 检查网络，确保能访问 `components.espressif.com` |
| 能连接但按键无效，设备管理器无键盘 | 电脑缓存了旧配对 / 描述符变更后未重配对 | 删除旧配对重启电脑后重新配对 |
| 换电脑 / 配对反复失败 | 设备侧残留旧绑定密钥 | 设备上长按上键、3 秒内再长按下键重置配对（见 README「蓝牙配对重置」）；或 `idf.py -p COM3 erase-flash` 全擦后重新烧录 |
| 改配置后行为诡异 | 切换协议栈后未全量清理 | `rd /s /q build && del sdkconfig && idf.py build` |
| 烧录失败 | USB 线仅充电 / 驱动未装 | 换数据线，安装 [CP210x/CH340 驱动](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/establish-serial-connection.html) |
| `ble_gatts_count_resources rc=3` | NOTIFY 特征手动加了 CCCD | 删除手动 CCCD，框架自动创建 |

## 关键实现细节

### 为什么是 Keyboard 而不是 Touch Digitizer，是 Bluedroid 而不是 NimBLE

TikTok Remote 使用 Touch Digitizer 是因为抖音只响应触摸手势。PPT 翻页只需标准键盘快捷键（方向键/F5/Esc），Keyboard HID 是最简单、兼容性最好的方案：

- 全平台免驱（Windows/macOS/Linux 原生支持）
- 全软件兼容（PowerPoint/WPS/LibreOffice/Keynote）

协议栈选择上，初期用 NimBLE 手写 GATT 在 Windows 上失败（能连接但识别不了键盘），
最终改用 ESP-IDF 官方 Bluedroid `esp_hidd` profile，完整演进过程见 [开发日志](development-log.md)。

### LVGL 线程安全

所有 UI 操作必须持锁，且锁内不做延时/阻塞：

```c
if (bsp_lvgl_lock(500)) {
    lv_label_set_text(...);
    bsp_lvgl_unlock();
}
```

### 启动顺序

`nvs_flash_init()` → I2C/电量计 → 显示/按键 → UI → 截屏监听 → BLE HID（NVS 必须在蓝牙之前）。

## 扩展方向

- **低功耗休眠**：无操作自动休眠，按键唤醒
- **组合键**：长按上/下键跳到首页/末页
- **PC 端配套软件**（Phase 2）：通过自定义 GATT Service 回传 PPT 标题、页码等信息到设备屏幕
- **中文字库**：转换点阵字体供 LVGL 使用，显示中文 PPT 标题

## 参考资料

- [USB HID Usage Tables](https://www.usb.org/document-library/hid-usage-tables-15)
- [ESP-IDF esp_hid 文档](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/api-reference/bluetooth/esp_hid.html)
- [ESP-IDF BLE HID Device 官方示例](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/ble/ble_hid_device_demo)
- [ESP-IDF 入门指南 (ESP32-C3)](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32c3/get-started/)
- [FoloToy AI Passport 官方仓库](https://github.com/FoloToy/ai-passport)
- [ai-passport-tiktok-remote 架构参考](https://github.com/YeatsLiao/ai-passport-tiktok-remote)
