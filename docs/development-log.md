# 开发日志

## 2026-08-31 — 项目初始化

### 目标
基于 AI Passport 硬件，实现 BLE HID Keyboard PPT 翻页遥控器。

### 设计决策

**为什么选 HID Keyboard 而不是 Touch Digitizer？**
- TikTok Remote 使用 Touch Digitizer 是因为抖音只响应触摸手势
- PPT 翻页只需标准键盘快捷键（方向键/F5/Esc），Keyboard HID 最简单
- Keyboard HID 全平台免驱，PowerPoint/WPS/LibreOffice/Keynote 都兼容

**按键映射选择**
- Left/Right Arrow：所有 PPT 软件都支持方向键翻页
- F5：PowerPoint/WPS/LibreOffice 通用的放映快捷键（Keynote 用 Cmd+Shift+Return，但 F5 在 macOS 上也被支持）
- Escape：通用的退出放映键

**演讲计时器**
- 纯设备端实现，不需要 PC 配套软件
- 按 F5 时自动启动，按 Esc 时自动停止
- 显示格式：MM:SS（超过 1 小时自动切换为 HH:MM:SS）

### 架构
复用 TikTok Remote 的命令队列模式：
- 按键回调 → FreeRTOS Queue → HID Task
- HID Task 发送 8 字节 Keyboard Report（按下 → 延时 → 释放）
- LVGL 定时器负责状态刷新和计时器更新

---

## 2026-08-31 — 蓝牙概念扫盲：各层选择到底有什么区别？

做蓝牙遥控器涉及 4 层选择，每层都可能踩坑：

```
┌─────────────────────────────────────────────────┐
│  第1层：蓝牙类型（两条不同的传输线路）               │
│  ├── 经典蓝牙 (BR/EDR)：老线路，带宽大，传统键盘用   │
│  │   ❌ ESP32-C3 硬件不支持                       │
│  └── BLE（低功耗蓝牙）：新线路，省电                │
│      ✅ ESP32-C3 只支持这个（别无选择）             │
├─────────────────────────────────────────────────┤
│  第2层：HID 角色（设备伪装成什么）                  │
│  ├── 键盘 Keyboard：发按键码 ← 本项目选择          │
│  ├── 鼠标 Mouse：发移动/点击                      │
│  └── 触摸板 Touch Digitizer：发手指绝对坐标         │
│      ← TikTok 遥控器选择（抖音只认触摸）            │
├─────────────────────────────────────────────────┤
│  第3层：协议栈（ESP-IDF 内置两套蓝牙主机栈）         │
│  ├── NimBLE：轻量（约 50KB RAM），API 原始          │
│  └── Bluedroid：全功能（约 100KB+ RAM），           │
│      Android 同源，自带 HID 设备 profile           │
├─────────────────────────────────────────────────┤
│  第4层：实现方式                                  │
│  ├── 手写 GATT：自己拼服务/特征/描述符 ← 失败      │
│  └── 官方 esp_hidd profile：乐鑫官方模板 ← 成功    │
└─────────────────────────────────────────────────┘
```

**为什么 PPT 选键盘而不是触摸？**
- PPT 翻页 = 标准键盘快捷键（←/→/F5/Esc），键盘 HID 最直接
- 键盘 HID 全平台免驱：Windows/macOS/Linux + PowerPoint/WPS/LibreOffice/Keynote
- 触摸 Digitizer 在电脑端会被当成平板触摸，不能驱动 PPT 翻页

**最终组合**：BLE + 键盘 HID + Bluedroid + 官方 esp_hidd profile。
这是乐鑫官方示例（`examples/bluetooth/bluedroid/ble/ble_hid_device_demo`）
验证过的组合，在 Windows 10/11 上可正常识别为键盘。

---

## 2026-08-31 — Windows 兼容性踩坑全记录（重要）

### 现象

设备能与 Windows 配对、能连接，固件日志显示 notify 发送成功，
但**设备管理器里不出现键盘设备，按键对电脑完全无效**。

### 尝试过的方案（均失败）

| 轮次 | 方案 | 结果 |
|------|------|------|
| 1 | NimBLE 手写 Keyboard GATT（8字节 Boot 报告，无 Report ID） | 连接成功，按键无效 |
| 2 | 补 Report Reference 描述符 (0x2908) + Output Report 特征 | 同上 |
| 3 | 加 Report ID（输入=1/输出=2），报告前缀 Report ID 字节 | 同上 |
| 4 | HID Information Flags 改为 RemoteWake\|NormallyConnectable | 同上 |

每轮都严格按流程：重新编译 → 烧录 → 电脑删除旧配对 → 重新配对，
设备管理器「键盘」分类始终不出现新设备。结论：
**Windows 收到了 notify 数据，但没把设备识别为 HID 键盘**——
手写 GATT 的某些细节不满足 Windows HID 驱动的要求，而这些细节在文档中并不明确。

### 最终解法（✅ 已在 Windows 实机验证成功）

放弃手写 GATT，改用 ESP-IDF 官方 HID 组件。注意 **IDF 版本演进导致的 API 变化**：

| API | 状态 | 说明 |
|-----|------|------|
| `esp_hidd_prf_api.h`（旧 Bluedroid HIDD profile） | ❌ IDF 5.5 已移除 | 旧版 `ble_hid_device_demo` 用的就是它，5.5.5 上编译报头文件不存在 |
| `esp_hid` 组件（`esp_hidd.h`） | ✅ 当前官方方案 | 新示例 `examples/bluetooth/esp_hid_device` |

当前实现（[ble_hid.c](../main/ble_hid.c)）：

- 头文件：`esp_hidd.h` / `esp_hidd_gatts.h` / `esp_hid_common.h`
- 初始化：控制器 + Bluedroid → `esp_ble_gap_register_callback` →
  `esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler)` →
  `esp_hidd_dev_init(&config, ESP_HID_TRANSPORT_BLE, cb, &dev)`
- 发按键：`esp_hidd_dev_input_set(dev, 0, report_id=1, buf8, 8)`（8 字节：modifier+reserved+6键，不带 Report ID 前缀）
- Report Map：标准键盘（Report ID=1 输入 + 1 字节 LED 输出），Appearance=Keyboard (0x03C1)
- 安全：`ESP_LE_AUTH_REQ_SC_BOND` + `ESP_IO_CAP_NONE`（Just Works 绑定，无需 PIN）
- 连接/断开由 `ESP_HIDD_CONNECT_EVENT`/`ESP_HIDD_DISCONNECT_EVENT` 驱动，断开后自动重新广播；
  配对认证在 GAP `ESP_GAP_BLE_AUTH_CMPL_EVT`（Windows 要求加密，认证完成前不发按键）
- 依赖：main 组件 `PRIV_REQUIRES esp_hid`；sdkconfig 需 `CONFIG_BT_BLUEDROID_ENABLED` +
  `CONFIG_BT_BLE_ENABLED`（SMP 默认开启）；
  `CONFIG_BT_HID_DEVICE_ENABLED` 是经典蓝牙 HID，ESP32-C3 不适用，不要开

### 连带修复的 bug：F5 键码错误

开发初期把 F5 的 HID Usage ID 写成了 `0x3F`，实际上是 **F6**。正确值：

| 键 | HID Usage ID |
|----|--------------|
| Escape | 0x29 |
| F1~F6 | 0x3A~0x3F（F5 = **0x3E**） |
| Right Arrow | 0x4F |
| Left Arrow | 0x50 |

### 经验教训（给后续开发者的提醒）

1. **ESP32 BLE 键盘在 Windows 上，直接用官方 esp_hid 组件，不要手写 NimBLE GATT**
2. **先确认 IDF 版本再选 API**：`esp_hidd_prf_api.h` 在 IDF 5.5+ 已移除，新代码必须用 `esp_hid` 组件；官方示例位置也从 `bluedroid/ble/ble_hid_device_demo` 换成了 `bluetooth/esp_hid_device`
3. 切换蓝牙协议栈（NimBLE ↔ Bluedroid）后，必须删除 `build/` 和 `sdkconfig` 全量重建，
   否则旧配置残留会导致诡异问题（判断方法：观察日志中 GATT att_handle 编号是否变化）
4. 修改 HID 描述符/服务结构后，必须在电脑蓝牙里**删除旧配对**再重新配对，
   Windows 会缓存旧设备的 GATT 信息
5. ESP32-C3 只支持 BLE，不支持经典蓝牙（BR/EDR），无法走传统蓝牙 HID 路线

---

## 2026-08-31 — macOS 无法进入放映：F5 不是 Mac 的放映快捷键（已修复）

### 现象与原因

设备在 Windows 全部正常，但接 macOS 时确认键无法进入放映。
原因：**F5 只是 Windows 系软件的约定**（PowerPoint/WPS/LibreOffice），
macOS 各家软件的放映快捷键完全不同：

| 软件 | 开始放映快捷键 |
|------|----------------|
| Windows PowerPoint / WPS / LibreOffice | F5 |
| macOS PowerPoint | ⌘⇧⏎ (Cmd+Shift+Return) |
| macOS Keynote | ⌥⌘P (Option+Cmd+P) |

BLE 键盘只能发按键，无法感知对端是什么系统。

### 解法：一次击键发三套快捷键（`ble_hid_press_start_slideshow`）

确认键短按时依次发送：
1. `F5`
2. `Cmd+Shift+Return`（修饰键位图 0x0A + Return 0x28）
3. `Alt+Cmd+P`（修饰键位图 0x0C + P 0x13）

未命中的组合在对应平台上均无快捷键绑定，无副作用（如 Windows 上
Win+Shift+Enter / Win+Alt+P 均无默认行为）。各击键间隔 80ms 避免被吞。

新增 API：`ble_hid_key_press_mod(modifier, keycode)` 支持带修饰键击键；
修饰键位图遵循 HID Usage Tables（L-Shift=0x02, L-Alt=0x04, L-GUI/Cmd=0x08）。

---

## 2026-08-31 — 电量显示打磨与发布固件命名规范

### UI 打磨

- **右上角电量加 `BAT` 前缀**（`BAT 91%`），对齐 tiktok-remote 风格，含义更直观。

### 发布固件命名

```
ai-passport-ppt-full.bin
```

- `full` = 从 `0x0` 整片烧录的合并镜像（含 bootloader + 分区表 + 应用），
  由 `idf.py build && idf.py merge-bin` 生成 `build/merged-binary.bin` 后重命名。
- 当前版本 v1.0.0：翻页遥控 + 跨平台开始放映 + BAT 电量前缀。
  发布到 FoloToy AI Passport 社区的固件即此合并镜像。

---

## 2026-08-31 — macOS 放映快捷键仍无效：击键时序太短

### 现象与原因

固件已同时发送三套快捷键（F5 / ⌘⇧⏎ / ⌥⌘P），组合本身正确：

- macOS PowerPoint 开始放映 = `⌘⇧⏎` (Cmd+Shift+Return)
- macOS Keynote 播放幻灯片 = `⌥⌘P` (Option+Cmd+P)，没错，就是 Option+Command+P

但 macOS 上依然无法进入放映。根因不在快捷键，而在**击键时序**：
按下保持仅 50ms、组合键间隔仅 80ms。单键（翻页）在 macOS 没问题，
但修饰键组合需要主机在多个 HID 轮询周期内稳定看到同一份报告才能
完整解析出 Shift+Cmd+Return，保持太短会被 macOS 直接丢弃；Windows 容忍度更高所以正常。

### 修复

- 按下保持 50ms → **120ms**（`KEY_HOLD_MS`）
- 组合键间隔 80ms → **200ms**（`KEY_COMBO_GAP_MS`）
- 三套快捷键总耗时约 1.4s，属于可接受范围；未命中平台的组合仍无副作用
