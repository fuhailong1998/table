# E-Ink 桌面信息显示器 — 开发笔记

## 项目概述

一个基于 ESP32-S3 + 5.83 寸墨水屏的桌面信息聚合显示器，实时展示加密货币价格、股指行情、黄金价格、汇率、天气、空气质量、恐惧贪婪指数、人体存在检测等信息。

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
- 增加了 GOOD / FAIR / POOR 舒适度文字状态
- 气压与舒适度并列显示在室内区底部

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
> | Binance 主站 | HTTP 451 | 中国大陆 IP 地区封锁 |
> | OKX | SSL -1 | SSL 握手失败，在某些网络环境下不可用 |
> | CryptoCompare | HTTP 401 | 无 Key 的旧接口已停止服务 |
> | **Binance Vision** | **成功** | **只读行情域，单次请求四币，约 1.2 KB** |
> | CoinGecko | 可用 | 单次请求四币，含 24h 涨跌，作为第一备源 |
> | jsDelivr currency-api | 可用 | 日线价格，作为无涨跌幅的最终兜底 |
>
> **最终方案**: Binance Vision (主) → CoinGecko (备) → jsDelivr (兜底)
>
> ```
> https://data-api.binance.vision/api/v3/ticker/24hr?symbols=%5B%22BTCUSDT%22,%22ETHUSDT%22,%22SOLUSDT%22,%22DOGEUSDT%22%5D&type=MINI
> ```
>
> Binance 返回数组的顺序不固定，固件按 `symbol` 映射币种，并用
> `openPrice` 与 `lastPrice` 计算滚动 24h 涨跌。四个币种全部通过校验后才更新缓存，
> 避免半成功响应混入上一轮数据。

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
> **最终方案**: 优先使用 open.er-api 返回的 XAU 汇率；缺失时回退到 jsDelivr CDN。
>
> ```
> https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/xau.min.json
> ```

### 阶段 6：天气 + 恐惧贪婪 + AQI

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

### 阶段 7：LD2402 毫米波雷达集成

**目标**: 检测人体存在，实现离开模式和工位占用统计。

**硬件**: HLK-LD2402-24G 毫米波雷达传感器，通过 UART 连接。

| LD2402 引脚 | ESP32-S3 GPIO |
|-------------|---------------|
| TX | GPIO4 (ESP32 RX) |
| RX | GPIO5 (ESP32 TX) |

> **踩坑 8: LD2402 协议不是二进制帧**
>
> 最初按照网上文档实现了二进制帧解析器（0xFD 帧头 + 长度 + 数据），但串口收到的数据完全无法解析。
>
> 添加原始 hex dump 后发现，该模块输出的是 **ASCII 文本**：
> - `"ON\r\n"` — 检测到人
> - `"OFF\r\n"` — 无人
> - `"distance:XXX\r\n"` — 目标距离（厘米）
>
> **教训**: 不同批次/固件版本的模块可能使用不同协议。始终先 dump 原始数据确认格式。

> **踩坑 9: 毫米波雷达近距离精度**
>
> LD2402 在 30cm 以内的距离测量不准确（即使人贴着传感器也显示 30-50cm）。
> 这是毫米波雷达的固有特性——最小检测距离约 30cm，属于正常行为。

**功能实现**:
- **状态显示**: Footer 区域显示 `RADAR --` / `SEATED` / `MOVING` / `AWAY` 与距离
- **工位占用统计**: 记录当天累计有人时长，每日午夜自动清零
- **离开模式**: 无人超过 60 秒后切换简洁大字时钟画面，有人回来自动恢复

### 阶段 8：股指行情

**目标**: 显示 S&P 500、纳斯达克、道琼斯、上证指数的实时行情。

**数据源**: 腾讯简版行情接口（免费、无需 Key）

```
https://qt.gtimg.cn/q=s_usINX,s_usIXIC,s_usDJI,s_sh000001
```

接口单次返回四条 JavaScript 赋值文本，以 `~` 分隔字段。响应标注为 GBK，
但变量名、数字和分隔符均为 ASCII，因此固件不需要转码：字段 3 是当前价格，
字段 5 是已经换算好的涨跌百分比。

HTTP 200 不保证四个代码都存在。固件按变量名映射并逐项校验；某项缺失时保留该项旧缓存，
不会用 `0` 覆盖屏幕上的有效行情。

| 代码 | 指数 |
|------|------|
| s_usINX | S&P 500 |
| s_usIXIC | 纳斯达克综合 |
| s_usDJI | 道琼斯工业 |
| s_sh000001 | 上证综合 |

### 阶段 9：UI 细节优化

界面改为适合单色墨水屏的编辑式信息仪表盘：

- 去掉贴边外框和密集卡片，使用 16px 内边距、留白和细分隔线建立层级。
- Header 只保留 Wi-Fi 状态、星期、日期和大时间；信号强度使用四格基础图元表示，不再显示可能很长的 SSID。
- 左栏固定展示 OUTDOOR、今明预报、AQI、INDOOR、气压和舒适度；右栏使用 `FreeMonoBold9pt7b` 对齐价格与涨跌幅。
- 行情的 11 个数据槽位使用固定基线。尚未取得有效数据时显示 `--` 或 `NO DATA`，不会因条件折叠导致整列跳动。
- jsDelivr 最终兜底源只提供日线价格，使用该源时涨跌列留空，不把缺失数据渲染成 `0.0%`。
- Gold 与 USD/CNY 的数据源没有真实 24h 涨跌幅，因此保持涨跌列为空，不再显示虚假的 `+0.0%`。
- Footer 分为 Presence、Distance、Today at desk 三列；雷达从未收到数据时显示 `RADAR --`，不会误报为离开。
- FreeSans 只覆盖 ASCII，屏幕文案统一使用英文；离开模式也不再尝试绘制中文字形。

**局刷对齐**:

GxEPD2 在 rotation 0/2 下会把局刷窗口的横向坐标扩展到 8px 边界。新版使用 Header `(0,0,648,88)`、左栏 `(0,88,288,336)`、右栏 `(288,88,360,336)`、Footer `(0,424,648,56)`，所有窗口均准确对齐，避免相邻区域被意外清白。

---

## 屏幕布局

```
┌──────────────────────────────────────────────────────────────┐
│  [||||] ONLINE  Tue                                          │
│  2026.08.09                                      17:30       │
├───────────────────────────┬──────────────────────────────────┤
│  OUTDOOR ───────────────  │  MARKETS ─────────────────────   │
│  -8°C       CLOUDY        │  ASSET      PRICE          24H   │
│             AQI 42  GOOD  │  BTC      $117420       ▲ +2.1%  │
│             PM2.5 12      │  ETH        $4210       ▼ -0.5%  │
│  TODAY     Cloud  -12/-3  │  SOL         $182       ▲ +1.3%  │
│  TMRW      Snow   -15/-6  │  DOGE      $0.214       — +0.0%  │
│  INDOOR ────────────────  │  ─────────────────────────────   │
│  TEMP          HUMIDITY   │  F&G           45   Extreme Fear │
│  23.4°C        45%        │  GOLD       $3380                │
│  PRESSURE      COMFORT    │  USD/CNY    7.1832               │
│  1013 hPa      GOOD       │  INDICES ─────────────────────   │
│                           │  SPX        6389.2       ▲ +0.8% │
│                           │  NDQ         22180       ▼ -0.3% │
│                           │  DJI         45211       ▲ +0.1% │
│                           │  SHC        3364.1       ▲ +0.5% │
├───────────────────────────┴──────────────────────────────────┤
│  PRESENCE          DISTANCE          TODAY AT DESK           │
│  SEATED            1.2 m             3h 28m                  │
└──────────────────────────────────────────────────────────────┘
```

**离开模式时钟画面**:
```
┌──────────────────────────────────────────────────────────────┐
│                         AWAY MODE                            │
│                                                              │
│                           17:30                              │
│                    2026.08.09 / Tue                          │
│                  INDOOR  23.4 C / 45%                        │
├──────────────────────────────────────────────────────────────┤
│                    TODAY AT DESK  3h 28m                     │
└──────────────────────────────────────────────────────────────┘
```

**刷新策略**:
- 全屏刷新: 每 30 分钟（防止墨水屏残影）
- 局部刷新: 各区域独立刷新，避免全屏闪烁
  - 时间区: 每 1 分钟
  - 传感器区: 每 2 分钟
  - 加密货币: 每 2 分钟
  - 天气/汇率/金价/F&G/AQI/股指: 每 10 分钟
- 达到离开阈值时立即切换为大字时钟，之后每 1 分钟刷新
- 检测到人员返回时立即全刷，恢复完整仪表盘并清除离开画面残影
- 30 分钟全刷优先于普通轮询；其他到期任务每轮只执行一个，避免连续局刷造成闪屏

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
加密货币: Binance Vision (主) → CoinGecko (备) → jsDelivr (兜底)
金价: open.er-api.com XAU 汇率 + jsDelivr CDN
```

每个源失败后自动尝试下一个。行情先解析到局部候选值，完整校验后才覆盖缓存；
请求失败时继续显示上一轮有效数据。

### 5. 自绘图形

墨水屏不依赖图标字体。温湿度/行情趋势箭头和 Wi-Fi 信号格通过 Adafruit GFX 基础图形函数绘制：

```cpp
void drawArrowUp(int cx, int cy, int size) {
    display.fillTriangle(cx, cy - size, cx - size, cy + size, cx + size, cy + size, GxEPD_BLACK);
}
```

FreeSans 字体只包含 ASCII，因此汇率标签使用 `USD/CNY`，避免额外引入 `¥` 字形或中文字库。

### 6. 分隔文本解析（腾讯股指）

腾讯行情返回 GBK 编码的 JavaScript 赋值文本，而不是 JSON。中文名称不参与解析，
只读取 ASCII 变量名与 `~` 分隔的数字字段：

```cpp
// v_s_usINX="200~...~.INX~7757.64~47.68~0.62~...";
// 字段 3 = 当前价格，字段 5 = 涨跌百分比
```

解析时用 `strtof` 检查完整数值，并用涨跌额反推昨收，对服务端给出的百分比做容差校验。

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
| 加密货币 | `data-api.binance.vision` | 无需 | 单请求四币，计算滚动 24h 涨跌 |
| 加密货币 (备) | `api.coingecko.com` | 无需 | 单请求四币 + 24h 涨跌 |
| 加密货币 (兜底) | `cdn.jsdelivr.net` (currency-api) | 无需 | 日线价格，无涨跌幅 |
| 汇率 | `open.er-api.com/v6/latest/USD` | 无需 | 含 XAU 汇率 |
| 金价 | `cdn.jsdelivr.net` (currency-api) | 无需 | 数据可能滞后 1-2 天 |
| 股指 | `qt.gtimg.cn` | 无需 | 单请求四指数，GBK 分隔文本 |
| 天气 | `api.open-meteo.com/v1/forecast` | 无需 | 免费无限制 |
| AQI | `air-quality-api.open-meteo.com` | 无需 | 免费无限制 |
| 恐惧贪婪 | `api.alternative.me/fng` | 无需 | |

---

## 踩坑总结

| # | 问题 | 现象 | 原因 | 解决方案 |
|---|------|------|------|----------|
| 1 | 串口无输出 | 上传成功但无任何打印 | 使用了 USB 口而非 UART 口，或 CDC 配置错误 | 使用 UART 口 + `ARDUINO_USB_CDC_ON_BOOT=0` |
| 2 | NTP 同步超时 | DNS 解析成功但 NTP 30 秒超时 | 运营商/路由器封锁 UDP 123 | 使用路由器本地 NTP 服务 |
| 3 | Binance 主站 API 451 | HTTP 451 响应 | 中国大陆 IP 被封 | 改用只读行情域 Binance Vision |
| 4 | OKX SSL 失败 | SSL 握手 -1 | 网络环境/DNS 问题 | 弃用 OKX，改用 Binance Vision |
| 5 | CryptoCompare 401 | 返回 `API key required` | 旧无 Key 接口停止服务 | 从请求链移除，改用 Binance Vision |
| 6 | Stooq 股指 404 | 四个指数全部请求失败 | 旧 CSV 路径失效 | 改用腾讯简版行情单请求 |
| 7 | DNS 解析失败 | 域名解析失败 | 路由器 DNS 污染 | 配置阿里 DNS 覆盖 |
| 8 | 黄金 API 全部失败 | SSL/403/数据过时 | 各 API 在中国网络环境不可用 | 使用 jsDelivr CDN |
| 9 | 墨水屏局刷竖线消失 | 局部刷新后分隔线不见 | 局部刷新窗口未覆盖竖线位置 | 调整窗口坐标并重绘竖线 |
| 10 | 墨水屏闪烁 | 1 秒刷新导致持续闪烁 | 墨水屏不适合高频刷新 | 改回 1 分钟刷新，去掉秒显示 |
| 11 | LD2402 协议错误 | 二进制解析器无法工作 | 模块输出 ASCII 而非二进制帧 | Hex dump 确认后改为 ASCII 解析 |
| 12 | 雷达近距离不准 | 贴着传感器仍显示 30-50cm | 毫米波雷达最小检测距离 ~30cm | 正常行为，无法改善 |
| 13 | 布局重叠 | DOGE 标签与价格重叠 | 4 字符标签比 3 字符宽 | 使用等宽数字字体与右对齐价格列 |
| 14 | ¥ 符号不显示 | FreeSans 字体无 ¥ | 字体仅含 ASCII 0x20-0x7E | 界面使用 `USD/CNY` ASCII 标签 |
| 15 | WSL 串口 | 无法访问 /dev/ttyUSB0 | WSL2 不直接暴露 USB | 使用 usbipd + udev 规则 |

---

## 资源占用

### 编译时（固件大小）

| 段 | 大小 | 说明 |
|----|------|------|
| RAM | ~86 KB / 320 KB (26.2%) | 栈、堆、BSS（含 GxEPD2 帧缓冲） |
| Flash | ~981 KB / 6.4 MB (15.0%) | 代码 + 数据 + 字体 |

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
| 加密货币 (Binance Vision) | ~1.2 KB | 每 2 分钟 | ~36 KB |
| 汇率 + 金价 | ~2 KB + ~1 KB | 每 10 分钟 | ~18 KB |
| 股指 (腾讯单请求) | ~0.33 KB | 每 10 分钟 | ~2 KB |
| 天气 | ~2 KB | 每 10 分钟 | ~12 KB |
| AQI | ~1 KB | 每 10 分钟 | ~6 KB |
| 恐惧贪婪 | ~0.5 KB | 每 10 分钟 | ~3 KB |
| **合计** | | | **~77 KB/h ≈ 1.8 MB/天** |

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
- [ ] Web 配置界面（WiFi/地理位置等）
- [ ] 3D 打印外壳
- [ ] 低功耗模式（电池供电场景）
- [ ] 更多数据源：RSS 新闻标题、日历事件等
- [ ] 黄金价格实时 API（当前 jsDelivr CDN 有延迟）
- [ ] USD/CNY 24h 涨跌幅（需要可用的数据源）
