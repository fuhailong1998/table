/**
 * 墨水屏桌面摆件 - 阶段4: 加密货币 + 汇率
 *
 * 功能:
 *   - 日期时间（NTP 同步）
 *   - BME280 温湿度/气压
 *   - BTC/ETH/SOL 价格（Binance API）
 *   - USD/CNY 汇率
 *   - 局部刷新 + 定期全刷
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <Adafruit_BME280.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "config.h"

// ============================================================
// 硬件初始化
// ============================================================

#define USE_GDEQ0583T31
#ifdef USE_GDEQ0583T31
GxEPD2_BW<GxEPD2_583_GDEQ0583T31, GxEPD2_583_GDEQ0583T31::HEIGHT>
    display(GxEPD2_583_GDEQ0583T31(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#else
GxEPD2_BW<GxEPD2_583_T8, GxEPD2_583_T8::HEIGHT>
    display(GxEPD2_583_T8(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
#endif

Adafruit_BME280 bme;
bool bmeReady = false;

// ============================================================
// 数据模型
// ============================================================

struct SensorData {
    float temperature, humidity, pressure;
    float prevTemp, prevHumid;
    bool valid, hasPrev;
} sensorData = {};

struct TimeData {
    int year, month, day, hour, minute, second, weekday;
    bool valid;
} timeData = {};

struct CryptoData {
    float btc, eth, sol, doge;
    float btcPct, ethPct, solPct, dogePct;   // 24h 涨跌幅
    bool valid;
} cryptoData = {};

struct FxData {
    float usdcny;
    float pct24h;
    bool valid, hasPct;
} fxData = {};

struct FearGreedData {
    int value;              // 0-100
    char label[16];         // "Fear", "Greed" etc.
    bool valid;
} fgData = {};

struct AqiData {
    int aqi;
    float pm25, pm10;
    bool valid;
} aqiData = {};

struct CursorData {
    int used, total;         // plan included
    int odUsedCents, odLimitCents;  // on-demand (cents)
    bool valid;
} cursorData = {};

struct WeatherData {
    float currentTemp;
    int   currentCode;
    float todayHigh, todayLow;
    int   todayCode;
    float tmrwHigh, tmrwLow;
    int   tmrwCode;
    bool  valid;
} weatherData = {};

struct GoldData {
    float priceUSD;
    float pct24h;
    bool valid;
} goldData = {};

struct IndexItem {
    char   name[8];     // "SPX", "NDQ", "DJI", "SHC"
    float  price;
    float  pctChange;   // daily change %
    bool   valid;
};

struct IndexData {
    IndexItem items[4];
    bool valid;
} indexData = {
    {{"SPX", 0, 0, false}, {"NDQ", 0, 0, false},
     {"DJI", 0, 0, false}, {"SHC", 0, 0, false}},
    false
};

struct RadarData {
    uint8_t status;        // 0=无人, 1=运动, 2=静止
    float   distanceCm;    // 距离(cm)
    bool    present;       // 是否有人
    unsigned long lastSeen;       // 上次检测到人的时间
    unsigned long todayOccupied;  // 今日在座累计毫秒
    int           lastResetDay;   // 上次重置统计的日期
    bool    awayMode;      // 是否进入了离开模式(简洁时钟)
} radarData = {};

HardwareSerial radarSerial(1);

// ============================================================
// 定时器
// ============================================================

unsigned long lastSensorRead   = 0;
unsigned long lastTimeRefresh  = 0;
unsigned long lastCryptoFetch  = 0;
unsigned long lastFxFetch      = 0;
unsigned long lastCursorFetch  = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastFgFetch      = 0;
unsigned long lastAqiFetch     = 0;
unsigned long lastIndexFetch   = 0;
unsigned long lastFullRefresh  = 0;
unsigned long lastOccupiedTick = 0;
bool wifiConnected = false;

// ============================================================
// LD2402 雷达
// ============================================================

// LD2402 使用 ASCII 串口协议，格式：
// "ON\r\n"        = 有人
// "OFF\r\n"       = 无人
// "distance:XXX\r\n" = 距离(cm)

static char radarLine[64];
static int radarLineIdx = 0;
static unsigned long lastRadarDebug = 0;
static unsigned long radarBytesTotal = 0;

void processRadarLine(const char *line)
{
    bool wasPresent = radarData.present;

    if (strncmp(line, "ON", 2) == 0) {
        radarData.present = true;
        radarData.status = 1;
        radarData.lastSeen = millis();
        if (!wasPresent) Serial.println("Radar: person detected");
    } else if (strncmp(line, "OFF", 3) == 0) {
        radarData.present = false;
        radarData.status = 0;
        if (wasPresent) Serial.println("Radar: person left");
    } else if (strncmp(line, "distance:", 9) == 0) {
        int d = atoi(line + 9);
        if (d > 0) {
            radarData.distanceCm = d;
            radarData.present = true;
            radarData.status = 2;
            radarData.lastSeen = millis();
            if (!wasPresent) {
                Serial.printf("Radar: person at %dcm\n", d);
            }
        }
    }
}

void pollRadar()
{
    while (radarSerial.available()) {
        char c = (char)radarSerial.read();
        radarBytesTotal++;

        if (c == '\n' || c == '\r') {
            if (radarLineIdx > 0) {
                radarLine[radarLineIdx] = '\0';
                processRadarLine(radarLine);
                radarLineIdx = 0;
            }
        } else {
            if (radarLineIdx < (int)sizeof(radarLine) - 1) {
                radarLine[radarLineIdx++] = c;
            }
        }
    }
}

void updateOccupancy()
{
    unsigned long now = millis();

    // 每日零点重置统计
    if (timeData.valid && timeData.day != radarData.lastResetDay) {
        radarData.todayOccupied = 0;
        radarData.lastResetDay = timeData.day;
        Serial.printf("Occupancy reset for day %d\n", timeData.day);
    }

    // 累加在座时间
    if (radarData.present && lastOccupiedTick > 0) {
        radarData.todayOccupied += (now - lastOccupiedTick);
    }
    lastOccupiedTick = radarData.present ? now : 0;

    // 判断是否进入离开模式
    bool wasAway = radarData.awayMode;
    radarData.awayMode = !radarData.present &&
                         radarData.lastSeen > 0 &&
                         (now - radarData.lastSeen > RADAR_AWAY_MS);

    if (radarData.awayMode != wasAway) {
        Serial.printf("Away mode: %s\n", radarData.awayMode ? "ON" : "OFF");
    }
}

// ============================================================
// WiFi
// ============================================================

void connectWiFi()
{
    Serial.printf("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500); Serial.print(".");
    }
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
        Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

#ifdef CUSTOM_DNS1
        IPAddress dns1 CUSTOM_DNS1;
        IPAddress dns2 CUSTOM_DNS2;
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
        Serial.printf("DNS overridden to %s / %s\n", dns1.toString().c_str(), dns2.toString().c_str());
#else
        Serial.printf("Using default DNS: %s\n", WiFi.dnsIP().toString().c_str());
#endif
    } else {
        Serial.println("\nWiFi FAILED!");
    }
}

void ensureWiFi()
{
    if (WiFi.status() != WL_CONNECTED) {
        wifiConnected = false;
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) delay(500);
        wifiConnected = (WiFi.status() == WL_CONNECTED);
    }
}

// ============================================================
// NTP 时间同步
// ============================================================

void syncTime()
{
    Serial.println("Trying NTP via gateway...");
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER, "ntp.aliyun.com", "203.107.6.88");
    struct tm t;
    for (int i = 0; i < 20; i++) {
        if (getLocalTime(&t, 500)) {
            Serial.printf("NTP OK! %04d-%02d-%02d %02d:%02d:%02d\n",
                          t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
            return;
        }
        delay(500);
    }
    Serial.println("NTP failed!");
}

void updateTimeData()
{
    struct tm t;
    if (getLocalTime(&t, 1000)) {
        timeData = {t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday, true};
    }
}

const char* weekdayName(int wd)
{
    const char* n[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    return (wd >= 0 && wd <= 6) ? n[wd] : "???";
}

// ============================================================
// 传感器
// ============================================================

void readSensor()
{
    if (!bmeReady) return;
    if (sensorData.valid) {
        sensorData.prevTemp  = sensorData.temperature;
        sensorData.prevHumid = sensorData.humidity;
        sensorData.hasPrev   = true;
    }
    sensorData.temperature = bme.readTemperature();
    sensorData.humidity    = bme.readHumidity();
    sensorData.pressure    = bme.readPressure() / 100.0F;
    sensorData.valid       = true;
    Serial.printf("Sensor: %.1fC  %.1f%%  %.0fhPa\n",
                  sensorData.temperature, sensorData.humidity, sensorData.pressure);
}

// ============================================================
// API: 加密货币价格
// ============================================================

struct OKXResult { float price, pct24h; };

OKXResult fetchOKXPrice(WiFiClientSecure &client, const char* instId)
{
    OKXResult r = {0, 0};
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "https://www.okx.com/api/v5/market/ticker?instId=%s", instId);

    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            r.price = doc["data"][0]["last"].as<float>();
            float open24 = doc["data"][0]["open24h"].as<float>();
            if (open24 > 0) r.pct24h = (r.price - open24) / open24 * 100.0f;
        }
    } else {
        Serial.printf("  OKX %s failed: %d\n", instId, code);
    }
    http.end();
    return r;
}

bool fetchBinanceTicker(WiFiClientSecure &client, const char* symbol,
                        float &outPrice, float &outPct)
{
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.binance.com/api/v3/ticker/24hr?symbol=%s", symbol);
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();
    bool ok = false;
    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            outPrice = doc["lastPrice"].as<float>();
            outPct   = doc["priceChangePercent"].as<float>();
            ok = (outPrice > 0);
        }
    } else {
        Serial.printf("  Binance %s failed: %d\n", symbol, code);
    }
    http.end();
    return ok;
}

void fetchCryptoPrices()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();

    // --- 方案1: CryptoCompare (单次请求获取全部) ---
    Serial.println("Fetching crypto (CryptoCompare)...");
    {
        HTTPClient http;
        http.begin(client, "https://min-api.cryptocompare.com/data/pricemultifull?fsyms=BTC,ETH,SOL,DOGE&tsyms=USD");
        http.setTimeout(10000);
        int code = http.GET();
        if (code == 200) {
            String payload = http.getString();
            JsonDocument doc;
            if (!deserializeJson(doc, payload)) {
                JsonObject raw = doc["RAW"];
                cryptoData.btc     = raw["BTC"]["USD"]["PRICE"].as<float>();
                cryptoData.btcPct  = raw["BTC"]["USD"]["CHANGEPCT24HOUR"].as<float>();
                cryptoData.eth     = raw["ETH"]["USD"]["PRICE"].as<float>();
                cryptoData.ethPct  = raw["ETH"]["USD"]["CHANGEPCT24HOUR"].as<float>();
                cryptoData.sol     = raw["SOL"]["USD"]["PRICE"].as<float>();
                cryptoData.solPct  = raw["SOL"]["USD"]["CHANGEPCT24HOUR"].as<float>();
                cryptoData.doge    = raw["DOGE"]["USD"]["PRICE"].as<float>();
                cryptoData.dogePct = raw["DOGE"]["USD"]["CHANGEPCT24HOUR"].as<float>();
                if (cryptoData.btc > 0) {
                    cryptoData.valid = true;
                    Serial.printf("Crypto (CryptoCompare): BTC=$%.0f(%+.1f%%)  ETH=$%.1f(%+.1f%%)  SOL=$%.2f(%+.1f%%)  DOGE=$%.4f(%+.1f%%)\n",
                                  cryptoData.btc, cryptoData.btcPct,
                                  cryptoData.eth, cryptoData.ethPct,
                                  cryptoData.sol, cryptoData.solPct,
                                  cryptoData.doge, cryptoData.dogePct);
                }
            }
        } else {
            Serial.printf("CryptoCompare failed: %d\n", code);
        }
        http.end();
        if (cryptoData.valid) return;
    }

    // --- 方案2: Binance ---
    Serial.println("CryptoCompare failed, trying Binance...");
    float bp, bpct;
    if (fetchBinanceTicker(client, "BTCUSDT", bp, bpct)) {
        cryptoData.btc = bp; cryptoData.btcPct = bpct;
        fetchBinanceTicker(client, "ETHUSDT",  cryptoData.eth,  cryptoData.ethPct);
        fetchBinanceTicker(client, "SOLUSDT",  cryptoData.sol,  cryptoData.solPct);
        fetchBinanceTicker(client, "DOGEUSDT", cryptoData.doge, cryptoData.dogePct);
        cryptoData.valid = true;
        Serial.printf("Crypto (Binance): BTC=$%.0f(%+.1f%%)  ETH=$%.1f(%+.1f%%)  SOL=$%.2f(%+.1f%%)  DOGE=$%.4f(%+.1f%%)\n",
                      cryptoData.btc, cryptoData.btcPct,
                      cryptoData.eth, cryptoData.ethPct,
                      cryptoData.sol, cryptoData.solPct,
                      cryptoData.doge, cryptoData.dogePct);
        return;
    }

    // --- 方案3: CoinGecko ---
    Serial.println("Binance failed, trying CoinGecko...");
    {
        HTTPClient http;
        http.begin(client, "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,solana,dogecoin&vs_currencies=usd");
        http.setTimeout(10000);
        int code = http.GET();
        if (code == 200) {
            String payload = http.getString();
            JsonDocument doc;
            if (!deserializeJson(doc, payload)) {
                cryptoData.btc  = doc["bitcoin"]["usd"].as<float>();
                cryptoData.eth  = doc["ethereum"]["usd"].as<float>();
                cryptoData.sol  = doc["solana"]["usd"].as<float>();
                cryptoData.doge = doc["dogecoin"]["usd"].as<float>();
                cryptoData.valid = true;
                Serial.printf("Crypto (CoinGecko): BTC=$%.0f  ETH=$%.1f  SOL=$%.2f  DOGE=$%.4f\n",
                              cryptoData.btc, cryptoData.eth, cryptoData.sol, cryptoData.doge);
            }
        } else {
            Serial.printf("CoinGecko also failed: %d\n", code);
        }
        http.end();
    }

    if (!cryptoData.valid) Serial.println("Crypto: all sources failed!");
}

// ============================================================
// API: USD/CNY 汇率
// ============================================================

void fetchExchangeRate()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    Serial.println("Fetching FX + Gold...");
    http.begin(client, EXCHANGE_API);
    http.setTimeout(10000);
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            JsonObject rates = doc["rates"];

            if (rates.containsKey("CNY")) {
                fxData.usdcny = rates["CNY"].as<float>();
                fxData.valid  = true;
                Serial.printf("FX: USD/CNY = %.4f\n", fxData.usdcny);
            }

            // XAU（如果 API 包含）
            if (rates.containsKey("XAU")) {
                float xauRate = rates["XAU"].as<float>();
                if (xauRate > 0) {
                    goldData.priceUSD = 1.0f / xauRate;
                    goldData.valid = true;
                    Serial.printf("Gold (XAU): $%.1f/oz\n", goldData.priceUSD);
                }
            }
        }
    } else {
        Serial.printf("FX API failed: %d\n", code);
    }
    http.end();

}

// ============================================================
// API: 黄金价格
// ============================================================

void fetchGoldPrice()
{
    if (!wifiConnected || goldData.valid) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    Serial.println("Fetching gold price...");

    // jsDelivr CDN
    http.begin(client, "https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/xau.min.json");
    http.setTimeout(10000);
    int code = http.GET();
    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            float p = doc["xau"]["usd"].as<float>();
            if (p > 100) {
                goldData.priceUSD = p;
                goldData.valid = true;
                Serial.printf("Gold (CDN): $%.1f/oz\n", p);
            }
        }
    } else {
        Serial.printf("  CDN failed: %d\n", code);
    }
    http.end();

    if (!goldData.valid) Serial.println("Gold: all sources failed");
}

// ============================================================
// API: 天气预报 (OpenMeteo)
// ============================================================

const char* weatherCodeToStr(int code)
{
    if (code == 0)                return "Clear";
    if (code <= 3)                return "Cloudy";
    if (code <= 48)               return "Fog";
    if (code <= 55)               return "Drizzle";
    if (code <= 57)               return "FrzDriz";
    if (code <= 65)               return "Rain";
    if (code <= 67)               return "FrzRain";
    if (code <= 75)               return "Snow";
    if (code <= 77)               return "SnowGr";
    if (code <= 82)               return "Showers";
    if (code <= 86)               return "SnowSh";
    if (code == 95)               return "TStorm";
    if (code <= 99)               return "TStHail";
    return "???";
}

void fetchWeather()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    Serial.println("Fetching weather...");
    http.begin(client, WEATHER_API);
    http.setTimeout(10000);
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            weatherData.currentTemp = doc["current"]["temperature_2m"].as<float>();
            weatherData.currentCode = doc["current"]["weather_code"].as<int>();

            JsonArray maxT = doc["daily"]["temperature_2m_max"];
            JsonArray minT = doc["daily"]["temperature_2m_min"];
            JsonArray wCode = doc["daily"]["weather_code"];

            if (maxT.size() >= 2) {
                weatherData.todayHigh = maxT[0].as<float>();
                weatherData.todayLow  = minT[0].as<float>();
                weatherData.todayCode = wCode[0].as<int>();
                weatherData.tmrwHigh  = maxT[1].as<float>();
                weatherData.tmrwLow   = minT[1].as<float>();
                weatherData.tmrwCode  = wCode[1].as<int>();
            }
            weatherData.valid = true;
            Serial.printf("Weather: Now %.1fC (%s), Today %.0f/%.0f, Tmrw %.0f/%.0f\n",
                          weatherData.currentTemp, weatherCodeToStr(weatherData.currentCode),
                          weatherData.todayLow, weatherData.todayHigh,
                          weatherData.tmrwLow, weatherData.tmrwHigh);
        }
    } else {
        Serial.printf("Weather API failed: %d\n", code);
    }
    http.end();
}

// ============================================================
// API: 恐惧贪婪指数 (alternative.me)
// ============================================================

void fetchFearGreed()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    Serial.println("Fetching Fear & Greed...");
    http.begin(client, "https://api.alternative.me/fng/?limit=1");
    http.setTimeout(10000);
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            fgData.value = doc["data"][0]["value"].as<int>();
            strlcpy(fgData.label, doc["data"][0]["value_classification"] | "???", sizeof(fgData.label));
            fgData.valid = true;
            Serial.printf("F&G: %d (%s)\n", fgData.value, fgData.label);
        }
    } else {
        Serial.printf("F&G API failed: %d\n", code);
    }
    http.end();
}

// ============================================================
// API: 空气质量 (OpenMeteo)
// ============================================================

void fetchAqi()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    Serial.println("Fetching AQI...");
    http.begin(client, "https://air-quality-api.open-meteo.com/v1/air-quality?latitude="
               WEATHER_LAT "&longitude=" WEATHER_LON "&current=us_aqi,pm2_5,pm10");
    http.setTimeout(10000);
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            aqiData.aqi  = doc["current"]["us_aqi"].as<int>();
            aqiData.pm25 = doc["current"]["pm2_5"].as<float>();
            aqiData.pm10 = doc["current"]["pm10"].as<float>();
            aqiData.valid = true;
            Serial.printf("AQI: %d, PM2.5=%.1f, PM10=%.1f\n",
                          aqiData.aqi, aqiData.pm25, aqiData.pm10);
        }
    } else {
        Serial.printf("AQI API failed: %d\n", code);
    }
    http.end();
}

// ============================================================
// API: Cursor Usage (本地代理服务器)
// ============================================================

void fetchCursorUsage()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    Serial.println("Fetching Cursor usage...");
    http.begin(client, CURSOR_USAGE_URL);
    http.setTimeout(10000);
    http.addHeader("Cookie", CURSOR_COOKIE);

    int code = http.GET();
    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            JsonObject plan = doc["individualUsage"]["plan"];
            JsonObject od   = doc["individualUsage"]["onDemand"];
            cursorData.used        = plan["used"].as<int>();
            cursorData.total       = plan["limit"].as<int>();
            cursorData.odUsedCents  = od["used"].as<int>();
            cursorData.odLimitCents = od["limit"].as<int>();
            cursorData.valid = true;
            Serial.printf("Cursor: %d/%d req, OD $%.2f/$%.2f\n",
                          cursorData.used, cursorData.total,
                          cursorData.odUsedCents / 100.0f,
                          cursorData.odLimitCents / 100.0f);
        }
    } else {
        Serial.printf("Cursor API failed: %d\n", code);
    }
    http.end();
}

// ============================================================
// API: 股市指数 (Stooq CSV)
// ============================================================

void fetchOneIndex(WiFiClientSecure &client, int idx, const char* symbol)
{
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url),
             "https://stooq.com/q/l/?s=%%5e%s&f=sd2t2ohlcv&h&e=csv", symbol);
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code == 200) {
        String payload = http.getString();
        // CSV: header\nSymbol,Date,Time,Open,High,Low,Close,Volume
        int nl = payload.indexOf('\n');
        if (nl > 0) {
            String dataLine = payload.substring(nl + 1);
            dataLine.trim();
            // parse CSV fields: Symbol,Date,Time,Open,High,Low,Close,Volume
            float open_p = 0, close_p = 0;
            int commaCount = 0;
            int start = 0;
            for (int i = 0; i <= (int)dataLine.length(); i++) {
                if (i == (int)dataLine.length() || dataLine[i] == ',') {
                    String field = dataLine.substring(start, i);
                    if (commaCount == 3) open_p = field.toFloat();    // Open
                    if (commaCount == 6) close_p = field.toFloat();   // Close
                    start = i + 1;
                    commaCount++;
                }
            }
            if (close_p > 0 && open_p > 0) {
                indexData.items[idx].price = close_p;
                indexData.items[idx].pctChange = (close_p - open_p) / open_p * 100.0f;
                indexData.items[idx].valid = true;
            }
        }
    } else {
        Serial.printf("  Stooq %s failed: %d\n", symbol, code);
    }
    http.end();
}

void fetchStockIndices()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();

    Serial.println("Fetching stock indices...");
    const char* symbols[] = {"spx", "ndq", "dji", "shc"};
    for (int i = 0; i < 4; i++) {
        fetchOneIndex(client, i, symbols[i]);
    }
    indexData.valid = indexData.items[0].valid || indexData.items[3].valid;

    if (indexData.valid) {
        for (int i = 0; i < 4; i++) {
            if (indexData.items[i].valid) {
                Serial.printf("  %s: %.1f (%+.2f%%)\n",
                              indexData.items[i].name,
                              indexData.items[i].price,
                              indexData.items[i].pctChange);
            }
        }
    } else {
        Serial.println("Stock indices: all failed");
    }
}

// ============================================================
// UI 布局 (648 x 480)
//
//  +--------------------------------------------+
//  | Date  Weekday    WiFi       HH:MM          | 0-75 Header
//  |============================================|
//  | Weather & Sensor | Crypto / Gold / FX      | 75-420 Body
//  |==================|=========================|
//  | Cursor: 102/500 req            Up 2h30m    | 420-480 Footer
//  +--------------------------------------------+
// ============================================================

namespace L {
    const int16_t M       = 6;
    const int16_t HDR_H   = 75;
    const int16_t MID_DIV = 300;
    const int16_t FOOT_Y  = 420;
    const int16_t W       = 648;
    const int16_t H       = 480;
}

// --- Header: 日期 + 时间 ---
void drawTimeArea()
{
    char dateBuf[32], timeBuf[16];
    if (timeData.valid) {
        snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d  %s",
                 timeData.year, timeData.month, timeData.day, weekdayName(timeData.weekday));
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeData.hour, timeData.minute);
    } else {
        strcpy(dateBuf, "----/--/--  ---");
        strcpy(timeBuf, "--:--");
    }

    display.setFont(&FreeSans12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(14, 35);
    display.print(dateBuf);

    display.setFont(&FreeSansBold24pt7b);
    int16_t tx, ty; uint16_t tw, th;
    display.getTextBounds(timeBuf, 0, 0, &tx, &ty, &tw, &th);
    display.setCursor(L::W - tw - 18, 48);
    display.print(timeBuf);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(14, 64);
    if (wifiConnected) {
        char wifiBuf[48];
        String ssid = WiFi.SSID();
        int rssi = WiFi.RSSI();
        snprintf(wifiBuf, sizeof(wifiBuf), "%s %ddBm", ssid.length() > 0 ? ssid.c_str() : "WiFi", rssi);
        display.print(wifiBuf);
    } else {
        display.print("WiFi --");
    }
}

// 趋势箭头 (cx,cy 为箭头中心, sz 为大小)
void drawArrowUp(int cx, int cy, int sz)
{
    // ▲
    display.fillTriangle(cx, cy - sz, cx - sz, cy + sz, cx + sz, cy + sz, GxEPD_BLACK);
}

void drawArrowDown(int cx, int cy, int sz)
{
    // ▼
    display.fillTriangle(cx, cy + sz, cx - sz, cy - sz, cx + sz, cy - sz, GxEPD_BLACK);
}

void drawArrowFlat(int cx, int cy, int sz)
{
    // → 横线
    display.drawFastHLine(cx - sz, cy, sz * 2, GxEPD_BLACK);
    display.drawFastHLine(cx - sz, cy - 1, sz * 2, GxEPD_BLACK);
}

// --- Left Panel: 天气 + 室内传感器 ---
void drawLeftPanel()
{
    int x = 14;
    int y = L::HDR_H + 28;
    char buf[48];
    int panelW = L::MID_DIV - 14;

    display.setTextColor(GxEPD_BLACK);

    // --- Weather 区域 ---
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(x, y);
    display.print("Weather");
    y += 10;
    display.drawFastHLine(x, y, panelW - 10, GxEPD_BLACK);

    if (weatherData.valid) {
        // 当前温度 + 天气
        y += 32;
        display.setFont(&FreeSansBold18pt7b);
        snprintf(buf, sizeof(buf), "%.0f", weatherData.currentTemp);
        display.setCursor(x, y);
        display.print(buf);
        int16_t tx, ty2; uint16_t tw2, th2;
        display.getTextBounds(buf, x, y, &tx, &ty2, &tw2, &th2);
        display.drawCircle(x + tw2 + 5, y - 20, 3, GxEPD_BLACK);
        display.setFont(&FreeSans12pt7b);
        display.setCursor(x + tw2 + 16, y);
        display.printf("C  %s", weatherCodeToStr(weatherData.currentCode));

        // Today
        y += 35;
        display.setFont(&FreeSans12pt7b);
        snprintf(buf, sizeof(buf), "Today %s  %.0f/%.0f",
                 weatherCodeToStr(weatherData.todayCode),
                 weatherData.todayLow, weatherData.todayHigh);
        display.setCursor(x, y);
        display.print(buf);

        // Tomorrow
        y += 30;
        snprintf(buf, sizeof(buf), "Tmrw  %s  %.0f/%.0f",
                 weatherCodeToStr(weatherData.tmrwCode),
                 weatherData.tmrwLow, weatherData.tmrwHigh);
        display.setCursor(x, y);
        display.print(buf);
    } else {
        y += 32;
        display.setFont(&FreeSans12pt7b);
        display.setCursor(x, y);
        display.print("Loading...");
        y += 65;
    }

    // AQI 显示 (天气区域下方)
    if (aqiData.valid) {
        y += 30;
        display.setFont(&FreeSans12pt7b);
        const char* aqiLabel;
        if (aqiData.aqi <= 50)       aqiLabel = "Good";
        else if (aqiData.aqi <= 100) aqiLabel = "OK";
        else if (aqiData.aqi <= 150) aqiLabel = "Bad";
        else                         aqiLabel = "!!!";
        snprintf(buf, sizeof(buf), "AQI:%d(%s) PM2.5:%.0f", aqiData.aqi, aqiLabel, aqiData.pm25);
        display.setCursor(x, y);
        display.print(buf);
    }

    // --- 室内环境 ---
    y += 18;
    display.drawFastHLine(x, y, panelW - 10, GxEPD_BLACK);
    y += 26;
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(x, y);
    display.print("Indoor");
    y += 10;
    display.drawFastHLine(x, y, panelW - 10, GxEPD_BLACK);

    if (sensorData.valid) {
        float t = sensorData.temperature;
        float h = sensorData.humidity;

        // 综合舒适度评分: 0=舒适, 1=一般, 2=不适, 3=很差
        int tScore = (t >= 18 && t <= 26) ? 0 : (t >= 10 && t <= 30) ? 1 : 2;
        int hScore = (h >= 30 && h <= 60) ? 0 : (h >= 20 && h <= 80) ? 1 : 2;
        int comfort = tScore + hScore;

        y += 34;
        display.setFont(&FreeSansBold18pt7b);
        snprintf(buf, sizeof(buf), "%.1f", t);
        display.setCursor(x, y);
        display.print(buf);
        int16_t tx2, ty2; uint16_t tw2, th2;
        display.getTextBounds(buf, x, y, &tx2, &ty2, &tw2, &th2);
        display.drawCircle(x + tw2 + 5, y - 20, 3, GxEPD_BLACK);
        display.setFont(&FreeSans12pt7b);
        int tempEndX = x + tw2 + 16;
        display.setCursor(tempEndX, y);
        display.print("C");

        // 温度趋势箭头
        if (sensorData.hasPrev) {
            int arrowX = tempEndX + 30;
            int arrowY = y - 10;
            float diff = t - sensorData.prevTemp;
            if (diff > 0.2f)      drawArrowUp(arrowX, arrowY, 6);
            else if (diff < -0.2f) drawArrowDown(arrowX, arrowY, 6);
            else                   drawArrowFlat(arrowX, arrowY, 6);
        }

        y += 32;
        snprintf(buf, sizeof(buf), "%.1f%%", h);
        display.setCursor(x, y);
        display.print(buf);

        // 湿度趋势箭头
        if (sensorData.hasPrev) {
            int16_t tx3, ty3; uint16_t tw3, th3;
            display.getTextBounds(buf, x, y, &tx3, &ty3, &tw3, &th3);
            int arrowX = x + tw3 + 12;
            int arrowY = y - 8;
            float diff = h - sensorData.prevHumid;
            if (diff > 0.5f)      drawArrowUp(arrowX, arrowY, 5);
            else if (diff < -0.5f) drawArrowDown(arrowX, arrowY, 5);
            else                   drawArrowFlat(arrowX, arrowY, 5);
        }

        y += 28;
        snprintf(buf, sizeof(buf), "%.0f hPa", sensorData.pressure);
        display.setCursor(x, y);
        display.print(buf);

        // 画表情图标 (左面板右下角)
        int faceX = L::MID_DIV - 42;
        int faceY = L::FOOT_Y - 36;
        int r = 28;
        display.drawCircle(faceX, faceY, r, GxEPD_BLACK);
        display.drawCircle(faceX, faceY, r - 1, GxEPD_BLACK);

        // 眼睛
        display.fillCircle(faceX - 9, faceY - 7, 3, GxEPD_BLACK);
        display.fillCircle(faceX + 9, faceY - 7, 3, GxEPD_BLACK);

        // 嘴巴 (根据舒适度)
        if (comfort == 0) {
            for (int i = -12; i <= 12; i++) {
                int my = faceY + 8 + (i * i) / 24;
                display.drawPixel(faceX + i, my, GxEPD_BLACK);
                display.drawPixel(faceX + i, my + 1, GxEPD_BLACK);
            }
        } else if (comfort <= 2) {
            display.drawFastHLine(faceX - 10, faceY + 10, 20, GxEPD_BLACK);
            display.drawFastHLine(faceX - 10, faceY + 11, 20, GxEPD_BLACK);
        } else {
            for (int i = -12; i <= 12; i++) {
                int my = faceY + 18 - (i * i) / 24;
                display.drawPixel(faceX + i, my, GxEPD_BLACK);
                display.drawPixel(faceX + i, my + 1, GxEPD_BLACK);
            }
        }
    } else {
        y += 34;
        display.setFont(&FreeSans12pt7b);
        display.setCursor(x, y);
        display.print("No Sensor");
    }
}

// --- Right Panel: 统一字体，紧凑布局 ---
void drawRightPanel()
{
    int x = L::MID_DIV + 14;
    int y = L::HDR_H + 24;
    char buf[48];
    int rightEdge = L::W - 16;
    int priceX = x + 90;
    int lineH = 28;

    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSans12pt7b);

    // helper lambda: 画一行 name price %change arrow
    auto drawRow = [&](const char* name, const char* priceStr, float pct, bool hasPct) {
        display.setCursor(x, y);
        display.print(name);
        display.setCursor(priceX, y);
        display.print(priceStr);
        if (hasPct) {
            char pctBuf[16];
            snprintf(pctBuf, sizeof(pctBuf), "%+.1f%%", pct);
            int16_t tx2, ty2; uint16_t tw2, th2;
            display.getTextBounds(pctBuf, 0, 0, &tx2, &ty2, &tw2, &th2);
            display.setCursor(rightEdge - tw2 - 14, y);
            display.print(pctBuf);
            if (pct > 0.05f) drawArrowUp(rightEdge - 6, y - 5, 3);
            else if (pct < -0.05f) drawArrowDown(rightEdge - 6, y - 5, 3);
        }
        y += lineH;
    };

    // --- Crypto ---
    if (cryptoData.valid) {
        struct { const char* sym; float price; float pct; } coins[] = {
            {"BTC",  cryptoData.btc,  cryptoData.btcPct},
            {"ETH",  cryptoData.eth,  cryptoData.ethPct},
            {"SOL",  cryptoData.sol,  cryptoData.solPct},
            {"DOGE", cryptoData.doge, cryptoData.dogePct},
        };
        for (auto &c : coins) {
            if (c.price >= 100000)     snprintf(buf, sizeof(buf), "$%.0f", c.price);
            else if (c.price >= 1000)  snprintf(buf, sizeof(buf), "$%.1f", c.price);
            else if (c.price >= 1)     snprintf(buf, sizeof(buf), "$%.2f", c.price);
            else                       snprintf(buf, sizeof(buf), "$%.4f", c.price);
            drawRow(c.sym, buf, c.pct, true);
        }
    } else {
        const char* names[] = {"BTC", "ETH", "SOL", "DOGE"};
        for (auto &n : names) { drawRow(n, "------", 0, false); }
    }

    // --- F&G (紧接 DOGE 下方) ---
    if (fgData.valid) {
        snprintf(buf, sizeof(buf), "%d", fgData.value);
        display.setCursor(x, y);
        display.print("F&G");
        display.setCursor(priceX, y);
        display.print(buf);
        int16_t tx2, ty2; uint16_t tw2, th2;
        display.getTextBounds(fgData.label, 0, 0, &tx2, &ty2, &tw2, &th2);
        display.setCursor(rightEdge - tw2 - 4, y);
        display.print(fgData.label);
        y += lineH + 6;
    }

    // --- Gold ---
    if (goldData.valid) {
        snprintf(buf, sizeof(buf), "$%.0f", goldData.priceUSD);
        drawRow("Gold", buf, goldData.pct24h, true);
    } else {
        drawRow("Gold", "------", 0, false);
    }

    // --- USD/CNY (手动绘制 ¥ 符号) ---
    {
        display.setCursor(x, y);
        display.print("$/");
        int16_t tx1, ty1; uint16_t tw1, th1;
        display.getTextBounds("$/", 0, 0, &tx1, &ty1, &tw1, &th1);
        int yenX = x + tw1 + 2;
        int yenW = 14, yenH = 16;
        int yenTop = y - yenH;
        int yenMid = y - yenH / 2;
        for (int d = 0; d <= 1; d++) {
            display.drawLine(yenX + d, yenTop, yenX + yenW / 2 + d, yenMid, GxEPD_BLACK);
            display.drawLine(yenX + yenW + d, yenTop, yenX + yenW / 2 + d, yenMid, GxEPD_BLACK);
            display.drawLine(yenX + yenW / 2 + d, yenMid, yenX + yenW / 2 + d, y, GxEPD_BLACK);
        }
        display.drawFastHLine(yenX + 2, yenMid + 2, yenW - 4, GxEPD_BLACK);
        display.drawFastHLine(yenX + 2, yenMid + 3, yenW - 4, GxEPD_BLACK);
        display.drawFastHLine(yenX + 2, yenMid + 6, yenW - 4, GxEPD_BLACK);
        display.drawFastHLine(yenX + 2, yenMid + 7, yenW - 4, GxEPD_BLACK);

        if (fxData.valid) {
            snprintf(buf, sizeof(buf), "%.4f", fxData.usdcny);
        } else {
            snprintf(buf, sizeof(buf), "------");
        }
        display.setCursor(priceX, y);
        display.print(buf);
        if (fxData.hasPct) {
            char pctBuf[16];
            snprintf(pctBuf, sizeof(pctBuf), "%+.1f%%", fxData.pct24h);
            int16_t tx2, ty2; uint16_t tw2, th2;
            display.getTextBounds(pctBuf, 0, 0, &tx2, &ty2, &tw2, &th2);
            display.setCursor(rightEdge - tw2 - 14, y);
            display.print(pctBuf);
            if (fxData.pct24h > 0.05f) drawArrowUp(rightEdge - 6, y - 5, 3);
            else if (fxData.pct24h < -0.05f) drawArrowDown(rightEdge - 6, y - 5, 3);
        }
        y += lineH;
    }
    y += 6;

    // --- 股指 ---
    if (indexData.valid) {
        for (int i = 0; i < 4; i++) {
            auto &item = indexData.items[i];
            if (item.valid) {
                if (item.price >= 10000)
                    snprintf(buf, sizeof(buf), "%.0f", item.price);
                else
                    snprintf(buf, sizeof(buf), "%.1f", item.price);
                drawRow(item.name, buf, item.pctChange, true);
            } else {
                drawRow(item.name, "------", 0, false);
            }
        }
    }
}

// --- Footer: Cursor Usage + On-Demand + Uptime + 进度条 ---
void drawFooter()
{
    char buf[64];
    int16_t tx, ty; uint16_t tw, th;
    display.setTextColor(GxEPD_BLACK);

    // 进度条 (顶部)
    int barX = 14, barY = L::FOOT_Y + 6, barW = L::W - 28, barH = 10;
    if (cursorData.valid && cursorData.total > 0) {
        float pct = (float)cursorData.used / cursorData.total;
        if (pct > 1.0f) pct = 1.0f;
        display.drawRect(barX, barY, barW, barH, GxEPD_BLACK);
        int fillW = (int)(pct * (barW - 2));
        if (fillW > 0)
            display.fillRect(barX + 1, barY + 1, fillW, barH - 2, GxEPD_BLACK);
    }

    // 文字行 (进度条下方)
    int textY = barY + barH + 22;

    display.setFont(&FreeSans12pt7b);
    if (cursorData.valid) {
        snprintf(buf, sizeof(buf), "Cursor %d/%d", cursorData.used, cursorData.total);
    } else {
        strcpy(buf, "Cursor --/--");
    }
    display.setCursor(14, textY);
    display.print(buf);

    // On-Demand (中间)
    display.setFont(&FreeSans9pt7b);
    if (cursorData.valid) {
        snprintf(buf, sizeof(buf), "OD %d/%d",
                 cursorData.odUsedCents, cursorData.odLimitCents);
    } else {
        strcpy(buf, "OD --");
    }
    display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
    display.setCursor(L::W / 2 - tw / 2, textY);
    display.print(buf);

    // 雷达状态 + 距离 + 工位时间 (右侧)
    {
        unsigned long secs = radarData.todayOccupied / 1000;
        int oh = secs / 3600, om = (secs % 3600) / 60;
        if (radarData.present) {
            const char* rstat = (radarData.status == 2) ? "Sit" : "Mov";
            snprintf(buf, sizeof(buf), "%s %.1fm %dh%dm",
                     rstat, radarData.distanceCm / 100.0f, oh, om);
        } else {
            snprintf(buf, sizeof(buf), "Away %dh%dm", oh, om);
        }
        display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
        display.setCursor(L::W - tw - 14, textY);
        display.print(buf);
    }
}

// ============================================================
// 全屏 + 局部刷新
// ============================================================

void drawFullScreen()
{
    display.setRotation(EPD_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawRect(0, 0, L::W, L::H, GxEPD_BLACK);
        display.drawFastHLine(0, L::HDR_H, L::W, GxEPD_BLACK);
        display.drawFastHLine(0, L::FOOT_Y, L::W, GxEPD_BLACK);
        display.drawFastVLine(L::MID_DIV, L::HDR_H, L::FOOT_Y - L::HDR_H, GxEPD_BLACK);

        drawTimeArea();
        drawLeftPanel();
        drawRightPanel();
        drawFooter();
    } while (display.nextPage());
    Serial.println("Full refresh done.");
}

void drawTimePartial()
{
    display.setPartialWindow(0, 0, L::W, L::HDR_H);
    display.firstPage();
    do {
        display.fillRect(0, 0, L::W, L::HDR_H, GxEPD_WHITE);
        display.drawFastHLine(0, 0, L::W, GxEPD_BLACK);
        display.drawFastVLine(0, 0, L::HDR_H, GxEPD_BLACK);
        display.drawFastVLine(L::W - 1, 0, L::HDR_H, GxEPD_BLACK);
        drawTimeArea();
    } while (display.nextPage());
}

void drawLeftPartial()
{
    int16_t px = 1, py = L::HDR_H + 1;
    uint16_t pw = L::MID_DIV - 1, ph = L::FOOT_Y - L::HDR_H - 1;
    display.setPartialWindow(px, py, pw, ph);
    display.firstPage();
    do {
        display.fillRect(px, py, pw, ph, GxEPD_WHITE);
        display.drawFastVLine(L::MID_DIV - 1, L::HDR_H, L::FOOT_Y - L::HDR_H, GxEPD_BLACK);
        drawLeftPanel();
    } while (display.nextPage());
}

void drawRightPartial()
{
    int16_t px = L::MID_DIV, py = L::HDR_H + 1;
    uint16_t pw = L::W - L::MID_DIV, ph = L::FOOT_Y - L::HDR_H - 1;
    display.setPartialWindow(px, py, pw, ph);
    display.firstPage();
    do {
        display.fillRect(px + 1, py, pw - 1, ph, GxEPD_WHITE);
        display.drawFastVLine(L::MID_DIV, L::HDR_H, L::FOOT_Y - L::HDR_H, GxEPD_BLACK);
        drawRightPanel();
    } while (display.nextPage());
}

void drawFooterPartial()
{
    int16_t py = L::FOOT_Y + 1;
    uint16_t ph = L::H - L::FOOT_Y - 1;
    display.setPartialWindow(1, py, L::W - 2, ph);
    display.firstPage();
    do {
        display.fillRect(1, py, L::W - 2, ph, GxEPD_WHITE);
        drawFooter();
    } while (display.nextPage());
}

// ============================================================
// Setup & Loop
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== E-Paper Desktop Monitor - Phase 6 ===");

    radarSerial.begin(RADAR_BAUD, SERIAL_8N1, RADAR_TX_PIN, RADAR_RX_PIN);
    Serial.println("LD2402 radar UART OK");

    SPI.begin(EPD_SCLK, -1, EPD_MOSI, EPD_CS);
    Wire.begin(BME_SDA, BME_SCL);

    if (bme.begin(BME_ADDR, &Wire)) {
        bmeReady = true;
        Serial.println("BME280 OK");
    } else {
        Serial.println("BME280 NOT found!");
    }

    connectWiFi();

    if (wifiConnected) {
        syncTime();
        fetchCryptoPrices();
        fetchExchangeRate();
        fetchGoldPrice();
        fetchWeather();
        fetchFearGreed();
        fetchAqi();
        fetchCursorUsage();
        fetchStockIndices();
    }

    readSensor();
    updateTimeData();

    display.init(115200, true, 50, false);
    drawFullScreen();

    unsigned long t = millis();
    lastFullRefresh  = t;
    lastSensorRead   = t;
    lastTimeRefresh  = t;
    lastCryptoFetch  = t;
    lastFxFetch      = t;
    lastWeatherFetch = t;
    lastFgFetch      = t;
    lastAqiFetch     = t;
    lastCursorFetch  = t;
    lastIndexFetch   = t;

    Serial.println("Phase 6 ready.");
}

void drawClockScreen()
{
    display.setRotation(EPD_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        char buf[32];
        updateTimeData();

        // 大字时钟居中
        display.setFont(&FreeSansBold24pt7b);
        snprintf(buf, sizeof(buf), "%02d:%02d", timeData.hour, timeData.minute);
        int16_t tx, ty; uint16_t tw, th;
        display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
        display.setCursor((L::W - tw) / 2 - tx, L::H / 2 - 20);
        display.print(buf);

        // 日期
        static const char* weekNames[] = {"日","一","二","三","四","五","六"};
        display.setFont(&FreeSans12pt7b);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s",
                 timeData.year, timeData.month, timeData.day,
                 weekNames[timeData.weekday % 7]);
        display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
        display.setCursor((L::W - tw) / 2 - tx, L::H / 2 + 30);
        display.print(buf);

        // 温度
        if (sensorData.valid) {
            snprintf(buf, sizeof(buf), "%.1fC  %.0f%%", sensorData.temperature, sensorData.humidity);
            display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
            display.setCursor((L::W - tw) / 2 - tx, L::H / 2 + 70);
            display.print(buf);
        }

        // 今日在座
        unsigned long secs = radarData.todayOccupied / 1000;
        int oh = secs / 3600, om = (secs % 3600) / 60;
        display.setFont(&FreeSans9pt7b);
        snprintf(buf, sizeof(buf), "Today %dh%dm", oh, om);
        display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
        display.setCursor((L::W - tw) / 2 - tx, L::H - 20);
        display.print(buf);
    } while (display.nextPage());
}

void loop()
{
    unsigned long now = millis();

    pollRadar();
    updateOccupancy();

    bool wasAway = radarData.awayMode;

    // 从离开模式回到正常模式 → 全刷
    if (wasAway && !radarData.awayMode) {
        // 刚回来，恢复完整画面
    }

    // 离开模式：只显示简洁时钟，降低刷新频率
    if (radarData.awayMode) {
        if (now - lastTimeRefresh >= 60000) {
            updateTimeData();
            readSensor();
            display.init(115200, false, 50, false);
            drawClockScreen();
            lastTimeRefresh = now;
        }
        delay(1000);
        return;
    }

    // === 正常模式 ===

    // 每分钟：刷新时间 + Footer
    if (now - lastTimeRefresh >= 60000) {
        updateTimeData();
        display.init(115200, false, 50, false);
        drawTimePartial();
        drawFooterPartial();
        lastTimeRefresh = now;
    }

    // 每 2 分钟：读传感器 + 局部刷新左面板
    if (now - lastSensorRead >= SENSOR_READ_INTERVAL) {
        readSensor();
        display.init(115200, false, 50, false);
        drawLeftPartial();
        lastSensorRead = now;
    }

    // 每 2 分钟：获取加密货币+金价 + 局部刷新右面板
    if (now - lastCryptoFetch >= CRYPTO_FETCH_INTERVAL) {
        ensureWiFi();
        fetchCryptoPrices();
        display.init(115200, false, 50, false);
        drawRightPartial();
        lastCryptoFetch = now;
    }

    // 每 10 分钟：获取汇率 + 金价
    if (now - lastFxFetch >= EXCHANGE_FETCH_INTERVAL) {
        ensureWiFi();
        goldData.valid = false;
        fetchExchangeRate();
        fetchGoldPrice();
        display.init(115200, false, 50, false);
        drawRightPartial();
        lastFxFetch = now;
    }

    if (now - lastWeatherFetch >= WEATHER_FETCH_INTERVAL) {
        ensureWiFi();
        fetchWeather();
        display.init(115200, false, 50, false);
        drawLeftPartial();
        lastWeatherFetch = now;
    }

    // 每 10 分钟：恐惧贪婪 + AQI
    if (now - lastFgFetch >= FG_FETCH_INTERVAL) {
        ensureWiFi();
        fetchFearGreed();
        fetchAqi();
        display.init(115200, false, 50, false);
        drawLeftPartial();
        drawRightPartial();
        lastFgFetch = now;
        lastAqiFetch = now;
    }

    // 每 5 分钟：获取 Cursor Usage
    if (now - lastCursorFetch >= CURSOR_FETCH_INTERVAL) {
        ensureWiFi();
        fetchCursorUsage();
        lastCursorFetch = now;
    }

    // 每 10 分钟：获取股指
    if (now - lastIndexFetch >= INDEX_FETCH_INTERVAL) {
        ensureWiFi();
        fetchStockIndices();
        display.init(115200, false, 50, false);
        drawRightPartial();
        lastIndexFetch = now;
    }

    // 每 30 分钟：全屏刷新（防残影）
    if (now - lastFullRefresh >= FULL_REFRESH_INTERVAL) {
        ensureWiFi();
        readSensor();
        updateTimeData();
        fetchCryptoPrices();
        goldData.valid = false;
        fxData.hasPct = false;
        fetchExchangeRate();
        fetchGoldPrice();
        fetchWeather();
        fetchFearGreed();
        fetchAqi();
        fetchCursorUsage();
        display.init(115200, true, 50, false);
        drawFullScreen();
        lastFullRefresh  = now;
        lastSensorRead   = now;
        lastTimeRefresh  = now;
        lastCryptoFetch  = now;
        lastFxFetch      = now;
        lastWeatherFetch = now;
        lastFgFetch      = now;
        lastAqiFetch     = now;
        lastCursorFetch  = now;
    }

    delay(1000);
}
