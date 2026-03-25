# E-Ink 桌面信息显示器 — 开发笔记

## 项目概述

一个基于 ESP32-S3 + 5.83 寸墨水屏的桌面信息聚合显示器，实时展示加密货币价格、黄金价格、汇率、天气、空气质量、恐惧贪婪指数、Cursor IDE 用量等信息。

**硬件成本**: ~¥150（ESP32-S3 ~¥40 + 5.83寸墨水屏 ~¥90 + BME280 ~¥10）

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
> #define NTP_SERVER "192.168.100.1"  // 路由器 IP
> ```
>
> **教训**: 嵌入式设备的网络环境可能受限，不要假设所有协议和端口都可用。优先使用局域网内的服务。

### 阶段 4：加密货币 + 汇率

**目标**: 获取 BTC/ETH/SOL/DOGE 实时价格和 USD/CNY 汇率。

> **踩坑 5: Binance API 返回 451（地区封锁）**
>
> 最初使用 Binance API，但返回 HTTP 451（Unavailable For Legal Reasons），因为 Binance 对中国大陆 IP 进行了地区封锁。
>
> **解决方案**: 切换到 OKX API，无地区限制：
> ```
> https://www.okx.com/api/v5/market/ticker?instId=BTC-USDT
> ```

> **踩坑 6: DNS 解析失败**
>
> 切换到 OKX 后，DNS 解析 `www.okx.com` 失败。ESP32 默认使用 DHCP 分配的 DNS（通常是路由器），而路由器的 DNS 可能被污染或不稳定。
>
> **解决方案**: 手动覆盖 DNS 为 Google/Cloudflare：
> ```cpp
> IPAddress dns1(8, 8, 8, 8);
> IPAddress dns2(1, 1, 1, 1);
> WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
> ```

**汇率数据源**: `https://open.er-api.com/v6/latest/USD`（免费、无需 API Key）

**24h 涨跌幅**:
- 加密货币: OKX API 直接返回 24h 开盘价，计算涨跌比例
- 汇率: 通过 Yahoo Finance `USDCNY=X` 获取 `chartPreviousClose` 计算

### 阶段 5：黄金价格

**目标**: 获取黄金现货价格（XAU/USD）。

这个功能的 API 选择最为曲折，经历了多次失败。

> **踩坑 7: 黄金价格 API 的艰难之路**
>
> | API | 结果 | 问题 |
> |-----|------|------|
> | metals.live | SSL 错误 | ESP32 的 TLS 实现不兼容 |
> | goldprice.org | HTTP 403 | 反爬虫机制 |
> | exchangerate-api.com | 数据过时 | 每日更新，非实时 |
> | jsDelivr CDN (`@fawazahmed0/currency-api`) | 可用但延迟 | 数据滞后 1-2 天 |
> | OKX PAXG-USDT | 可用 | 黄金代币，与实际金价有 <1% 误差 |
> | **Yahoo Finance (`GC=F`)** | **成功** | **实时，含 24h 变化** |
>
> **最终方案**: Yahoo Finance 作为主源，OKX PAXG 作为备选，jsDelivr CDN 作为兜底。
>
> ```cpp
> // Yahoo Finance 金价
> https://query1.finance.yahoo.com/v8/finance/chart/GC=F?interval=1d&range=1d
> ```
>
> 解析 `meta.regularMarketPrice` 获取当前价格，`meta.chartPreviousClose` 计算 24h 涨跌。

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

---

## 屏幕布局

```
┌─────────────────────────────────────────────────┐
│  2026-03-25 周二                      17:30     │  ← 头部: 日期 + 时间
├───────────────────────┬─────────────────────────┤
│  28.5°C  ▲           │  --- Crypto ---          │
│  45.2%   ▼           │  BTC    $87654   +2.1%▲  │
│  1013 hPa            │  ETH    $2187    +1.5%▲  │
│  😊                   │  SOL    $142     +3.4%▲  │
│                       │  DOGE   $0.1742  +5.2%▲  │
│  --- Weather ---      │  --- Gold / FX ---       │
│  Now: 15°C ☀️         │  Gold   $3045    +0.8%▲  │
│  Today: 8/18°C       │  CNY    7.2341   -0.1%▼  │
│  Tmrw: 5/15°C        │  --- F&G ---             │
│                       │  45 Fear                  │
│                       │  AQI 85 PM2.5=35         │
├───────────────────────┴─────────────────────────┤
│  Cursor: 102/500 req  OD: 0/1000   Up 2h30m     │  ← 底部
└─────────────────────────────────────────────────┘
```

**刷新策略**:
- 全屏刷新: 每 30 分钟（防止墨水屏残影）
- 局部刷新: 各区域独立刷新，避免全屏闪烁
  - 时间区: 每 1 分钟
  - 传感器区: 每 2 分钟
  - 加密货币: 每 2 分钟
  - 天气/汇率/金价/F&G/AQI: 每 10 分钟
  - Cursor: 每 5 分钟

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

对关键数据（如黄金价格）采用多源容错：

```
Yahoo Finance (主) → OKX PAXG (备) → jsDelivr CDN (兜底)
```

每个源失败后自动尝试下一个。使用 `valid` 标志位避免重复请求已获取的数据。

### 5. 自绘图形

墨水屏不支持内置 emoji 字体，温度趋势箭头和舒适度表情通过 Adafruit GFX 基础图形函数手绘：

```cpp
void drawArrowUp(int cx, int cy, int size) {
    display.fillTriangle(cx, cy - size, cx - size, cy + size, cx + size, cy + size, GxEPD_BLACK);
}
```

---

## API 汇总

| 功能 | API | 认证 | 备注 |
|------|-----|------|------|
| 加密货币 | `www.okx.com/api/v5/market/ticker` | 无需 | 无地区限制 |
| 汇率 | `open.er-api.com/v6/latest/USD` | 无需 | 每日更新 |
| 汇率 24h | `query1.finance.yahoo.com/v8/finance/chart/USDCNY=X` | 无需 | 需 User-Agent |
| 金价 | `query1.finance.yahoo.com/v8/finance/chart/GC=F` | 无需 | 需 User-Agent |
| 天气 | `api.open-meteo.com/v1/forecast` | 无需 | 免费无限制 |
| AQI | `air-quality-api.open-meteo.com/v1/air-quality` | 无需 | 免费无限制 |
| 恐惧贪婪 | `api.alternative.me/fng` | 无需 | |
| Cursor | `cursor.com/api/usage-summary` | Cookie | 非公开 API |

---

## 踩坑总结

| # | 问题 | 现象 | 原因 | 解决方案 |
|---|------|------|------|----------|
| 1 | 串口无输出 | 上传成功但无任何打印 | 使用了 USB 口而非 UART 口，或 CDC 配置错误 | 使用 UART 口 + `ARDUINO_USB_CDC_ON_BOOT=0` |
| 2 | NTP 同步超时 | DNS 解析成功但 NTP 30 秒超时 | 运营商/路由器封锁 UDP 123 | 使用路由器本地 NTP 服务 |
| 3 | Binance API 451 | HTTP 451 响应 | 中国大陆 IP 被封 | 切换到 OKX API |
| 4 | OKX DNS 失败 | `www.okx.com` 解析失败 | 路由器 DNS 污染 | 手动指定 8.8.8.8 / 1.1.1.1 |
| 5 | 黄金 API 全部失败 | SSL 错误 / 403 / 数据过时 | 各种原因 | 三级容错: Yahoo → OKX PAXG → CDN |
| 6 | 墨水屏局刷竖线消失 | 局部刷新后分隔线不见 | 局部刷新窗口未覆盖竖线位置 | 调整窗口坐标并重绘竖线 |
| 7 | 墨水屏闪烁 | 1 秒刷新导致持续闪烁 | 墨水屏不适合高频刷新 | 改回 1 分钟刷新，去掉秒显示 |
| 8 | 布局重叠 | 新增内容与现有内容重叠 | 垂直空间不足 | 缩小行距、精简标签、调整间距 |
| 9 | WSL 串口 | 无法访问 /dev/ttyUSB0 | WSL2 不直接暴露 USB | 使用 usbipd + udev 规则 |

---

## 资源占用

### 编译时（固件大小）

| 段 | 大小 | 说明 |
|----|------|------|
| `.text` | 777,501 bytes (759 KB) | 代码段（程序指令） |
| `.data` | 209,160 bytes (204 KB) | 已初始化数据（字体、常量等） |
| `.bss` | 837,365 bytes (817 KB) | 未初始化数据（GxEPD2 帧缓冲区占大头） |
| **固件总大小** | **970,437 bytes (948 KB)** | Flash 中实际存储大小 |

### Flash 分区（16MB）

| 分区 | 大小 | 用途 |
|------|------|------|
| app0 | 6,291,456 bytes (6 MB) | 主程序（OTA 槽 0） |
| app1 | 6,291,456 bytes (6 MB) | OTA 槽 1（预留） |
| spiffs | 3,538,944 bytes (3.4 MB) | 文件系统（未使用） |
| nvs | 20,480 bytes (20 KB) | 非易失性存储 |
| coredump | 65,536 bytes (64 KB) | 崩溃转储 |

**Flash 使用率**: 948 KB / 6 MB = **14.8%**（仍有大量空间）

### RAM 使用

| 类型 | 总量 | 已用 | 使用率 | 说明 |
|------|------|------|--------|------|
| 内部 SRAM | 320 KB | 83.6 KB | 26.1% | 栈、堆、BSS（含 GxEPD2 帧缓冲） |
| PSRAM (OPI) | 8 MB | ~0 | <1% | 外部 PSRAM，当前未显式使用 |

> **`.bss` 为什么这么大（817 KB）？**
>
> GxEPD2 为 648×480 的 1-bit 帧缓冲区分配了约 `648 * 480 / 8 = 38,880 bytes`，
> 加上用于局部刷新的第二缓冲区、WiFi/TLS 协议栈的缓冲区等，合计约 817 KB。
> ESP32-S3 的内部 SRAM（320 KB）不足以容纳全部 BSS，部分会自动分配到 PSRAM。

### 运行时网络流量（估算）

| 数据 | 单次请求大小 | 频率 | 每小时流量 |
|------|-------------|------|-----------|
| 加密货币 (4 币种 × OKX) | ~1 KB × 4 | 每 2 分钟 | ~120 KB |
| 汇率 + 金价 | ~2 KB + ~3 KB | 每 10 分钟 | ~30 KB |
| 天气 | ~2 KB | 每 10 分钟 | ~12 KB |
| AQI | ~1 KB | 每 10 分钟 | ~6 KB |
| 恐惧贪婪 | ~0.5 KB | 每 10 分钟 | ~3 KB |
| Cursor | ~1 KB | 每 5 分钟 | ~12 KB |
| **合计** | | | **~183 KB/h ≈ 4.3 MB/天** |

### 依赖库版本

| 库 | 版本 | 用途 |
|----|------|------|
| GxEPD2 | 1.6.8 | 墨水屏驱动 |
| Adafruit GFX | 1.12.5 | 图形库 |
| Adafruit BME280 | 2.3.0 | 温湿度传感器 |
| ArduinoJson | 7.4.3 | JSON 解析 |
| U8g2_for_Adafruit_GFX | 1.8.0 | 字体渲染 |
| WiFiClientSecure | 2.0.0 | HTTPS 客户端 |
| HTTPClient | 2.0.0 | HTTP 请求 |

---

## 可改进方向

- [ ] 嵌入 CA 根证书替代 `setInsecure()`
- [ ] 添加 OTA (Over-The-Air) 远程更新
- [ ] Web 配置界面（WiFi/Cookie/地理位置等）
- [ ] 3D 打印外壳
- [ ] 低功耗模式（电池供电场景）
- [ ] 更多数据源：股票指数、RSS 新闻标题等
