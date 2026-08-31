// main/fap_screenshot.h — FAP_SCREENSHOT_V1 串口截屏协议
// 供 FoloToy AI Passport 发布助手读取运行中的屏幕画面
#pragma once

/**
 * @brief 启动串口截屏监听任务
 * 监听控制台输入 "FAP_SCREENSHOT_V1"，收到后回传当前屏幕帧缓冲
 */
void fap_screenshot_start(void);
