#pragma once

#include "secrets.h"

// ============================================================
// 墨水屏桌面摆件 - 硬件配置
// ============================================================

// --- 墨水屏 SPI 引脚 (5.83寸 648x480) ---
#define EPD_CS    10   // FSPI CS
#define EPD_MOSI  11   // FSPI MOSI (DIN)
#define EPD_SCLK  12   // FSPI CLK (SCLK)
#define EPD_DC    13   // 数据/命令
#define EPD_RST   14   // 复位
#define EPD_BUSY  15   // 忙检测

// --- BME280 I2C 引脚 ---
#define BME_SDA   17
#define BME_SCL   18
#define BME_ADDR  0x76  // SDO 接 GND 时地址为 0x76

// --- NTP 配置 ---
#define NTP_SERVER    "192.168.6.1"
#define GMT_OFFSET    8 * 3600      // UTC+8
#define DST_OFFSET    0

// --- DNS 覆盖（注释掉则使用路由器默认 DNS）---
#define CUSTOM_DNS1 (223, 5, 5, 5)
#define CUSTOM_DNS2 (119, 29, 29, 29)

// --- API 端点 ---
#define EXCHANGE_API  "https://open.er-api.com/v6/latest/USD"

// --- Cursor Usage ---
#define CURSOR_USAGE_URL "https://cursor.com/api/usage-summary"

// --- 天气配置（沈阳浑南区）---
#define WEATHER_LAT   "41.72"
#define WEATHER_LON   "123.46"
#define WEATHER_API   "https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON \
                      "&current=temperature_2m,weather_code" \
                      "&daily=temperature_2m_max,temperature_2m_min,weather_code" \
                      "&timezone=Asia%2FShanghai&forecast_days=2"

// --- 刷新间隔 (毫秒) ---
#define SENSOR_READ_INTERVAL      120000   // 传感器: 2 分钟
#define CRYPTO_FETCH_INTERVAL     120000   // 加密货币: 2 分钟
#define EXCHANGE_FETCH_INTERVAL   600000   // 汇率+金价: 10 分钟
#define WEATHER_FETCH_INTERVAL    600000   // 天气: 10 分钟
#define FG_FETCH_INTERVAL         600000   // 恐惧贪婪: 10 分钟
#define AQI_FETCH_INTERVAL        600000   // AQI: 10 分钟
#define CURSOR_FETCH_INTERVAL     300000   // Cursor: 5 分钟
#define INDEX_FETCH_INTERVAL      600000   // 股指: 10 分钟
#define FULL_REFRESH_INTERVAL     1800000  // 全刷新: 30 分钟

// --- LD2402 雷达传感器 (UART) ---
#define RADAR_TX_PIN  4     // LD2402 TX → ESP32 RX
#define RADAR_RX_PIN  5     // LD2402 RX → ESP32 TX
#define RADAR_BAUD    115200
#define RADAR_AWAY_MS 60000    // 无人切换时钟画面: 1 分钟

// --- 屏幕配置 ---
#define EPD_WIDTH    648
#define EPD_HEIGHT   480
#define EPD_ROTATION 2      // 0=默认, 2=翻转180度
