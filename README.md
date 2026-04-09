# E-Ink Desktop Monitor

基于 ESP32-S3 的墨水屏桌面信息显示器，集成多种实时数据展示。

## 功能

| 模块 | 数据源 | 刷新间隔 |
|------|--------|----------|
| 时间日期 | NTP 同步 | 1 分钟 |
| 温湿度/气压 | BME280 传感器 | 2 分钟 |
| BTC/ETH/SOL/DOGE 价格 + 24h 涨跌 | CryptoCompare / Binance / CoinGecko | 2 分钟 |
| 黄金现货价格 | jsDelivr CDN (currency-api) | 10 分钟 |
| USD/CNY 汇率 | open.er-api.com | 10 分钟 |
| 股指 (S&P500/纳斯达克/道琼斯/上证) | Stooq.com | 10 分钟 |
| 天气预报（今天/明天） | Open-Meteo | 10 分钟 |
| 恐惧贪婪指数 | alternative.me | 10 分钟 |
| 空气质量 AQI | Open-Meteo | 10 分钟 |
| Cursor IDE 用量 | Cursor API | 5 分钟 |
| 人体存在检测 + 工位占用统计 | LD2402 毫米波雷达 | 实时 |
| Wi-Fi 名称 + 信号强度 | ESP32 WiFi | 1 分钟 |

### 离开模式

当 LD2402 雷达检测到无人超过 1 分钟，自动切换为简洁大字时钟画面；检测到人回来后恢复完整信息显示。

## 硬件

- **MCU**: ESP32-S3-DevKitC-N16R8 (16MB Flash, 8MB PSRAM)
- **显示屏**: 5.83 寸 e-ink (648×480, 双色, 支持局部刷新)
- **传感器**: BME280 (温度/湿度/气压)
- **雷达**: HLK-LD2402-24G (毫米波人体存在检测)

### 接线

| 模块 | 引脚 | ESP32-S3 GPIO |
|------|------|---------------|
| E-Ink CS | CS | 10 |
| E-Ink DIN | MOSI | 11 |
| E-Ink CLK | SCLK | 12 |
| E-Ink DC | DC | 13 |
| E-Ink RST | RST | 14 |
| E-Ink BUSY | BUSY | 15 |
| BME280 SDA | SDA | 17 |
| BME280 SCL | SCL | 18 |
| LD2402 TX | RX | 4 |
| LD2402 RX | TX | 5 |

## 快速开始

### 1. 安装 PlatformIO

```bash
python3 -m venv .pio-venv
source .pio-venv/bin/activate
pip install platformio
```

### 2. 配置密钥

```bash
cp firmware/src/secrets.h.example firmware/src/secrets.h
```

编辑 `firmware/src/secrets.h`，填入：
- WiFi SSID 和密码
- Cursor 的 `WorkosCursorSessionToken` cookie（从浏览器 DevTools 获取）

### 3. 编译烧录

```bash
cd firmware
pio run -t upload
```

### 4. 查看串口输出

```bash
pio device monitor -b 115200
```

## 自定义

- **地理位置**: 修改 `config.h` 中的 `WEATHER_LAT` / `WEATHER_LON`
- **NTP 服务器**: 修改 `config.h` 中的 `NTP_SERVER`
- **刷新间隔**: 修改 `config.h` 中的 `*_INTERVAL` 常量
- **DNS 覆盖**: 修改 `config.h` 中的 `CUSTOM_DNS1` / `CUSTOM_DNS2`
- **屏幕旋转**: 修改 `config.h` 中的 `EPD_ROTATION`（0=默认, 2=翻转180°）
- **离开超时**: 修改 `config.h` 中的 `RADAR_AWAY_MS`（默认 60 秒）

## 项目结构

```
firmware/
├── platformio.ini          # PlatformIO 配置
└── src/
    ├── config.h            # 硬件引脚、API、刷新间隔配置
    ├── secrets.h           # WiFi 和 Cookie（不提交到 git）
    ├── secrets.h.example   # secrets.h 模板
    └── main.cpp            # 主程序
docs/
└── development-notes.md    # 开发笔记与踩坑记录
```

## License

MIT
