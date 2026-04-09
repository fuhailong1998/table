# E-Ink 桌面信息显示器 — 开发笔记

## 项目概述

一个基于 ESP32-S3 + 5.83 寸墨水屏的桌面信息聚合显示器，实时展示加密货币价格、股指行情、黄金价格、汇率、天气、空气质量、恐惧贪婪指数、Cursor IDE 用量、人体存在检测等信息。

**硬件成本**: ~¥170（ESP32-S3 ~¥40 + 5.83寸墨水屏 ~¥90 + BME280 ~¥10 + LD2402 ~¥30）

---

## 开发历程

### 阶段 1：硬件验证（Hello World）

**目标**: 验证 ESP32-S3 能驱动 5.83 寸墨水屏显示文字。

**技术选型**:
- 框架: Arduino (PlatformIO)
- 墨水屏驱动: GxEPD2 库（兼容微雪 5.83 V2）
- 传感器: Adafruit BME280 库

**接线方案**:

| E-Ink 引脚 | ESP32-S3 GPIO | 说明 |
|------------|---------------|------|
| CS | GPIO10 | FSPI CS |
| DIN (MOSI) | GPIO11 | FSPI MOSI |
| CLK (SCLK) | GPIO12 | FSPI CLK |
| DC | GPIO13 | 数据/命令选择 |
| RST | GPIO14 | 硬件复位 |
| BUSY | GPIO15 | 忙状态检测 |

| BME280 引脚 | ESP32-S3 GPIO |
|-------------|---------------|
| SDA | GPIO17 |
| SCL | GPIO18 |

> **踩坑 1: ESP32-S3 双 USB-C 端口**
>
> ESP32-S3-DevKitC 有两个 USB-C 口：
> - **USB** 口: 原生 USB，用于 USB CDC（需要 `ARDUINO_USB_CDC_ON_BOOT=1`）
> - **UART** 口: 通过 CP2102/CH340 芯片，映射为 `/dev/ttyUSB0`
>
> 开发时应使用 **UART** 口，并设置 `ARDUINO_USB_CDC_ON_BOOT=0`，否则串口无输出。

> **踩坑 2: PlatformIO 的 ESP32-S3 内存配置**
>
> N16R8 型号需要正确配置 Flash 和 PSRAM：
> ```ini
> board_build.arduino.memory_type = qio_opi
> board_build.flash_size = 16MB
> board_build.psram_type = opi
> board_build.partitions = default_16MB.csv
> ```
> 错误的内存配置会导致启动失败或 PSRAM 不可用。

> **踩坑 3: WSL 下的串口权限**
>
> WSL2 中需要通过 `usbipd` 将 USB 设备绑定到 WSL，然后添加 udev 规则：
> ```bash
> # Windows 端
> usbipd list
> usbipd bind --busid <BUS_ID>
> usbipd attach --wsl --busid <BUS_ID>
>
> # WSL 端
> sudo chmod 666 /dev/ttyUSB0
> ```

### 阶段 2：传感器数据显示

**目标**: 读取 BME280 温湿度气压并显示在墨水屏上。

BME280 通过 I2C 连接，SDO 接 GND 时地址为 `0x76`。使用 Adafruit BME280 库，初始化时需要指定正确的 I2C 引脚：

```cpp
Wire.begin(BME_SDA, BME_SCL);
bme.begin(BME_ADDR, &Wire);
```

**后续增强**:
- 增加了温湿度趋势箭头（与上次读数对比，显示▲▼—）
- 增加了舒适度表情图标（根据温湿度综合判断，用自绘 emoji 显示）
- 气压显示在湿度下方

### 阶段 3：WiFi + NTP 时间同步

**目标**: 连接 WiFi，通过 NTP 获取准确时间。

> **踩坑 4: NTP 同步失败（UDP 端口 123 被封）**
>
> 这是最折腾的问题之一。使用 `ntp.aliyun.com`、`pool.ntp.org` 等公网 NTP 服务器，DNS 解析成功但 NTP 同步始终超时。
>
> **原因**: 路由器/运营商封锁了 UDP 端口 123 的出站流量。
>
> **解决方案**: 使用路由器自带的 NTP 服务（OpenWrt/ImmortalWrt 默认开启 NTP 服务端）：
> ```cpp
> #define NTP_SERVER "192.168.6.1"  // 路由器 IP
> ```
>
> **教训**: 嵌入式设备的网络环境可能受限，不要假设所有协议和端口都可用。优先使用局域网内的服务。

### 阶段 4：加密货币 + 汇率

**目标**: 获取 BTC/ETH/SOL/DOGE 实时价格和 USD/CNY 汇率。

> **踩坑 5: 加密货币 API 的漫漫长路**
>
> | API | 结果 | 问题 |
> |-----|------|------|
> | Binance | HTTP 451 | 中国大陆 IP 地区封锁 |
> | OKX | SSL -1 | SSL 握手失败，在某些网络环境下不可用 |
> | **CryptoCompare** | **成功** | **单次请求获取全部币种，含 24h 涨跌** |
> | CoinGecko | 可用 | 作为兜底，无 24h 涨跌（免费版） |
>
> **最终方案**: CryptoCompare (主) → Binance (备) → CoinGecko (兜底)
>
> ```
> https://min-api.cryptocompare.com/data/pricemultifull?fsyms=BTC,ETH,SOL,DOGE&tsyms=USD
> ```

> **踩坑 6: DNS 解析失败**
>
> ESP32 默认使用 DHCP 分配的 DNS（通常是路由器），而路由器的 DNS 可能被污染或不稳定。
>
> **解决方案**: 在 `config.h` 中配置 DNS 覆盖（阿里 DNS）：
> ```cpp
> #define CUSTOM_DNS1 (223, 5, 5, 5)
> #define CUSTOM_DNS2 (119, 29, 29, 29)
> ```

**汇率数据源**: `https://open.er-api.com/v6/latest/USD`（免费、无需 API Key）

### 阶段 5：黄金价格

**目标**: 获取黄金现货价格（XAU/USD）。

这个功能的 API 选择最为曲折，经历了多次失败。

> **踩坑 7: 黄金价格 API 的艰难之路**
>
> | API | 结果 | 问题 |
> |-----|------|------|
> | metals.live | SSL 错误 | ESP32 的 TLS 实现不兼容 |
> | goldprice.org | HTTP 403 | 反爬虫机制 |
> | Yahoo Finance (`GC=F`) | SSL/403 | 网络环境下不可用 |
> | OKX PAXG-USDT | SSL -1 | 同加密货币 API 问题 |
> | **jsDelivr CDN (`@fawazahmed0/currency-api`)** | **可用** | **数据可能滞后 1-2 天** |
>
> **最终方案**: jsDelivr CDN 作为唯一来源。汇率 API 也可返回 XAU 汇率作为补充。
>
> ```
> https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/xau.min.json
> ```

### 阶段 6：Cursor IDE 用量

**目标**: 显示 Cursor IDE 的计划用量和按需用量。

> **踩坑 8: Cursor API 认证**
>
> Cursor 没有公开的 API 文档。通过浏览器 DevTools 抓包发现：
> - 端点: `https://cursor.com/api/usage-summary`
> - 认证: 通过 Cookie 中的 `WorkosCursorSessionToken`
>
> ESP32 通过 HTTPClient 发送带 Cookie 的 HTTPS 请求：
> ```cpp
> http.addHeader("Cookie", CURSOR_COOKIE);
> ```
>
> 响应包含 `individualUsage.plan.used/limit` 和 `individualUsage.onDemand.used/limit`。

### 阶段 7：天气 + 恐惧贪婪 + AQI

**天气预报**: Open-Meteo API（免费、无需 Key）
```
https://api.open-meteo.com/v1/forecast?latitude=41.72&longitude=123.46
  &current=temperature_2m,weather_code
  &daily=temperature_2m_max,temperature_2m_min,weather_code
  &timezone=Asia/Shanghai&forecast_days=2
```

**恐惧贪婪指数**: alternative.me API
```
https://api.alternative.me/fng/?limit=1
```

**空气质量 AQI**: Open-Meteo Air Quality API
```
https://air-quality-api.open-meteo.com/v1/air-quality?latitude=41.72&longitude=123.46
  &current=us_aqi,pm2_5,pm10
```

### 阶段 8：LD2402 毫米波雷达集成

**目标**: 检测人体存在，实现离开模式和工位占用统计。

**硬件**: HLK-LD2402-24G 毫米波雷达传感器，通过 UART 连接。

| LD2402 引脚 | ESP32-S3 GPIO |
|-------------|---------------|
| TX | GPIO4 (ESP32 RX) |
| RX | GPIO5 (ESP32 TX) |

> **踩坑 9: LD2402 协议不是二进制帧**
>
> 最初按照网上文档实现了二进制帧解析器（0xFD 帧头 + 长度 + 数据），但串口收到的数据完全无法解析。
>
> 添加原始 hex dump 后发现，该模块输出的是 **ASCII 文本**：
> - `"ON\r\n"` — 检测到人
> - `"OFF\r\n"` — 无人
> - `"distance:XXX\r\n"` — 目标距离（厘米）
>
> **教训**: 不同批次/固件版本的模块可能使用不同协议。始终先 dump 原始数据确认格式。

> **踩坑 10: 毫米波雷达近距离精度**
>
> LD2402 在 30cm 以内的距离测量不准确（即使人贴着传感器也显示 30-50cm）。
> 这是毫米波雷达的固有特性——最小检测距离约 30cm，属于正常行为。

**功能实现**:
- **状态显示**: Footer 区域显示 Sit（静坐）/ Mov（运动）/ Away（离开）+ 距离
- **工位占用统计**: 记录当天累计有人时长，每日午夜自动清零
- **离开模式**: 无人超过 60 秒后切换简洁大字时钟画面，有人回来自动恢复

### 阶段 9：股指行情

**目标**: 显示 S&P 500、纳斯达克、道琼斯、上证指数的实时行情。

**数据源**: Stooq.com CSV API（免费、无需 Key）

```
https://stooq.com/q/l/?s=^spx&f=sd2t2ohlcv&h&e=csv
```

CSV 返回格式：`Symbol,Date,Time,Open,High,Low,Close,Volume`

通过 `(Close - Open) / Open * 100` 计算日涨跌幅。

| 代码 | 指数 |
|------|------|
| ^spx | S&P 500 |
| ^ndq | 纳斯达克综合 |
| ^dji | 道琼斯工业 |
| 000001.ss | 上证综合 |

### 阶段 10：UI 细节优化

**右面板布局**:
- 去掉 "Crypto"、"Gold / FX" 等分区标题和分割线，统一使用 `FreeSans12pt7b` 字体
- 11 行数据紧凑排列：BTC → ETH → SOL → DOGE → F&G → Gold → $/¥ → SPX → NDQ → DJI → SHC
- F&G 描述（如 "Extreme Fear"）靠右对齐
- F&G 和 $/¥ 下方各增加 6px 间距，视觉分组

**¥ 符号手动绘制**:
FreeSans12pt7b 字体只包含 ASCII 0x20-0x7E，不含 ¥（U+00A5）。通过 `drawLine` + `drawFastHLine` 手动绘制加粗的 ¥ 符号（V形 + 竖线 + 两条横线，各线条 2px 宽度）。

**Wi-Fi 信息**: Header 显示实际连接的 SSID 名称和信号强度（dBm）。

**AQI 字体统一**: AQI 显示从 `FreeSans9pt7b` 改为 `FreeSans12pt7b`，与天气区域保持一致。

---

## 屏幕布局

```
┌─────────────────────────────────────────────────┐
│  2026-03-25 周二        OpenWrt -45dBm  17:30   │  ← 头部: 日期 + WiFi + 时间
├───────────────────────┬─────────────────────────┤
│  Weather              │  BTC    $87654   +2.1%▲  │
│  28°C  Sunny          │  ETH    $2187    +1.5%▲  │
│  Today Sunny  8/18    │  SOL    $142     +3.4%▲  │
│  Tmrw  Cloud  5/15    │  DOGE   $0.1742  +5.2%▲  │
│  AQI:42(Good) PM2.5:12│  F&G    45    Extreme Fear│
│  ─────────────────── │                           │
│  Indoor               │  Gold   $3045    +0.8%▲  │
│  28.5°C  ▲            │  $/¥    7.2341            │
│  45.2%   ▼            │                           │
│  1013 hPa             │  SPX    5842     +0.3%▲  │
│  😊                    │  NDQ    18742    -0.5%▼  │
│                        │  DJI    42356    +0.1%▲  │
│                        │  SHC    3245     +1.2%▲  │
├───────────────────────┴─────────────────────────┤
│  Cursor: 102/500 req  OD: 0/1000  Sit 1.2m 3h28m│  ← 底部: Cursor + 雷达
└─────────────────────────────────────────────────┘
```

**离开模式时钟画面**:
```
┌─────────────────────────────────────────────────┐
│                                                   │
│                     17:30                         │
│                  2026-03-25                       │
│                     周二                          │
│                                                   │
└─────────────────────────────────────────────────┘
```

**刷新策略**:
- 全屏刷新: 每 30 分钟（防止墨水屏残影）
- 局部刷新: 各区域独立刷新，避免全屏闪烁
  - 时间区: 每 1 分钟
  - 传感器区: 每 2 分钟
  - 加密货币: 每 2 分钟
  - 天气/汇率/金价/F&G/AQI/股指: 每 10 分钟
  - Cursor: 每 5 分钟
- 离开模式: 大字时钟每 1 分钟刷新

---

## 关键技术要点

### 1. E-Ink 局部刷新

GxEPD2 支持窗口化局部刷新，避免全屏闪烁：

```cpp
display.setPartialWindow(x, y, w, h);
display.firstPage();
do {
    // 绘制内容
} while (display.nextPage());
```

但局部刷新会逐渐产生残影，需要定期全刷清除。本项目设置 30 分钟全刷一次。

### 2. HTTPS 证书处理

ESP32 的 WiFiClientSecure 默认需要 CA 证书验证。为简化开发，使用 `setInsecure()` 跳过验证：

```cpp
WiFiClientSecure client;
client.setInsecure();
```

生产环境建议嵌入各 API 域名的根证书。

### 3. JSON 解析内存管理

ArduinoJson v7 使用 `JsonDocument` 自动管理内存。对于 ESP32-S3 的 8MB PSRAM，无需特别优化。但仍建议：
- 及时释放 HTTPClient 和 JsonDocument
- 避免同时持有多个大型 JSON 文档
- 使用 `http.getString()` 而非流式解析（ESP32 内存充足）

### 4. 多 API 容错

对关键数据采用多源容错：

```
加密货币: CryptoCompare (主) → Binance (备) → CoinGecko (兜底)
金价: open.er-api.com XAU 汇率 + jsDelivr CDN
```

每个源失败后自动尝试下一个。使用 `valid` 标志位避免重复请求已获取的数据。

### 5. 自绘图形

墨水屏不支持内置 emoji 字体，温度趋势箭头和舒适度表情通过 Adafruit GFX 基础图形函数手绘：

```cpp
void drawArrowUp(int cx, int cy, int size) {
    display.fillTriangle(cx, cy - size, cx - size, cy + size, cx + size, cy + size, GxEPD_BLACK);
}
```

¥ 符号也通过手动绘制实现（FreeSans 字体不含非 ASCII 字符）。

### 6. CSV 解析（Stooq 股指）

Stooq.com 返回 CSV 格式而非 JSON，需要手动解析：

```cpp
// 跳过 header 行，找到第二行的 Close 字段
int commaCount = 0;
for (char c : line) {
    if (c == ',') commaCount++;
    if (commaCount == 6) { /* 读取 Close 价格 */ }
}
```

### 7. LD2402 ASCII 协议

LD2402 模块（部分固件版本）输出 ASCII 文本而非二进制帧：

```
ON\r\n           → 检测到人体存在
OFF\r\n          → 无人
distance:150\r\n → 目标距离 150cm
```

通过逐行读取 UART 并字符串匹配处理。

---

## API 汇总

| 功能 | API | 认证 | 备注 |
|------|-----|------|------|
| 加密货币 | `min-api.cryptocompare.com` | 无需 | 单请求全币种 + 24h 涨跌 |
| 加密货币 (备) | `api.binance.com` | 无需 | 中国大陆可能 451 |
| 加密货币 (兜底) | `api.coingecko.com` | 无需 | 免费版无 24h 涨跌 |
| 汇率 | `open.er-api.com/v6/latest/USD` | 无需 | 含 XAU 汇率 |
| 金价 | `cdn.jsdelivr.net` (currency-api) | 无需 | 数据可能滞后 1-2 天 |
| 股指 | `stooq.com/q/l/` | 无需 | CSV 格式，含日开/收盘价 |
| 天气 | `api.open-meteo.com/v1/forecast` | 无需 | 免费无限制 |
| AQI | `air-quality-api.open-meteo.com` | 无需 | 免费无限制 |
| 恐惧贪婪 | `api.alternative.me/fng` | 无需 | |
| Cursor | `cursor.com/api/usage-summary` | Cookie | 非公开 API |

---

## 踩坑总结

| # | 问题 | 现象 | 原因 | 解决方案 |
|---|------|------|------|----------|
| 1 | 串口无输出 | 上传成功但无任何打印 | 使用了 USB 口而非 UART 口，或 CDC 配置错误 | 使用 UART 口 + `ARDUINO_USB_CDC_ON_BOOT=0` |
| 2 | NTP 同步超时 | DNS 解析成功但 NTP 30 秒超时 | 运营商/路由器封锁 UDP 123 | 使用路由器本地 NTP 服务 |
| 3 | Binance API 451 | HTTP 451 响应 | 中国大陆 IP 被封 | 切换到 CryptoCompare |
| 4 | OKX SSL 失败 | SSL 握手 -1 | 网络环境/DNS 问题 | 弃用 OKX，改用 CryptoCompare |
| 5 | DNS 解析失败 | 域名解析失败 | 路由器 DNS 污染 | 配置阿里 DNS 覆盖 |
| 6 | 黄金 API 全部失败 | SSL/403/数据过时 | 各 API 在中国网络环境不可用 | 使用 jsDelivr CDN |
| 7 | 墨水屏局刷竖线消失 | 局部刷新后分隔线不见 | 局部刷新窗口未覆盖竖线位置 | 调整窗口坐标并重绘竖线 |
| 8 | 墨水屏闪烁 | 1 秒刷新导致持续闪烁 | 墨水屏不适合高频刷新 | 改回 1 分钟刷新，去掉秒显示 |
| 9 | LD2402 协议错误 | 二进制解析器无法工作 | 模块输出 ASCII 而非二进制帧 | Hex dump 确认后改为 ASCII 解析 |
| 10 | 雷达近距离不准 | 贴着传感器仍显示 30-50cm | 毫米波雷达最小检测距离 ~30cm | 正常行为，无法改善 |
| 11 | 布局重叠 | DOGE 标签与价格重叠 | 4 字符标签比 3 字符宽 | 增大 priceX 偏移量 |
| 12 | ¥ 符号不显示 | FreeSans 字体无 ¥ | 字体仅含 ASCII 0x20-0x7E | 手动绘制 ¥ 符号 |
| 13 | WSL 串口 | 无法访问 /dev/ttyUSB0 | WSL2 不直接暴露 USB | 使用 usbipd + udev 规则 |

---

## 资源占用

### 编译时（固件大小）

| 段 | 大小 | 说明 |
|----|------|------|
| RAM | ~86 KB / 320 KB (26.2%) | 栈、堆、BSS（含 GxEPD2 帧缓冲） |
| Flash | ~979 KB / 6.4 MB (14.9%) | 代码 + 数据 + 字体 |

### Flash 分区（16MB）

| 分区 | 大小 | 用途 |
|------|------|------|
| app0 | 6,291,456 bytes (6 MB) | 主程序（OTA 槽 0） |
| app1 | 6,291,456 bytes (6 MB) | OTA 槽 1（预留） |
| spiffs | 3,538,944 bytes (3.4 MB) | 文件系统（未使用） |
| nvs | 20,480 bytes (20 KB) | 非易失性存储 |
| coredump | 65,536 bytes (64 KB) | 崩溃转储 |

### 运行时网络流量（估算）

| 数据 | 单次请求大小 | 频率 | 每小时流量 |
|------|-------------|------|-----------|
| 加密货币 (CryptoCompare) | ~3 KB | 每 2 分钟 | ~90 KB |
| 汇率 + 金价 | ~2 KB + ~1 KB | 每 10 分钟 | ~18 KB |
| 股指 (4×Stooq CSV) | ~0.5 KB × 4 | 每 10 分钟 | ~12 KB |
| 天气 | ~2 KB | 每 10 分钟 | ~12 KB |
| AQI | ~1 KB | 每 10 分钟 | ~6 KB |
| 恐惧贪婪 | ~0.5 KB | 每 10 分钟 | ~3 KB |
| Cursor | ~1 KB | 每 5 分钟 | ~12 KB |
| **合计** | | | **~153 KB/h ≈ 3.6 MB/天** |

### 依赖库版本

| 库 | 版本 | 用途 |
|----|------|------|
| GxEPD2 | 1.6.8 | 墨水屏驱动 |
| Adafruit GFX | 1.12.5 | 图形库 |
| Adafruit BME280 | 2.3.0 | 温湿度传感器 |
| ArduinoJson | 7.4.3 | JSON 解析 |
| U8g2_for_Adafruit_GFX | 1.8.0 | 字体渲染（预留） |
| WiFiClientSecure | 2.0.0 | HTTPS 客户端 |
| HTTPClient | 2.0.0 | HTTP 请求 |

---

## 可改进方向

- [ ] 嵌入 CA 根证书替代 `setInsecure()`
- [ ] 添加 OTA (Over-The-Air) 远程更新
- [ ] Web 配置界面（WiFi/Cookie/地理位置等）
- [ ] 3D 打印外壳
- [ ] 低功耗模式（电池供电场景）
- [ ] 更多数据源：RSS 新闻标题、日历事件等
- [ ] 黄金价格实时 API（当前 jsDelivr CDN 有延迟）
- [ ] USD/CNY 24h 涨跌幅（需要可用的数据源）
