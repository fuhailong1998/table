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
unsigned long lastFullRefresh  = 0;
bool wifiConnected = false;

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

    // --- 方案1: OKX ---
    Serial.println("Fetching crypto prices (OKX)...");
    OKXResult btc  = fetchOKXPrice(client, "BTC-USDT");
    OKXResult eth  = fetchOKXPrice(client, "ETH-USDT");
    OKXResult sol  = fetchOKXPrice(client, "SOL-USDT");
    OKXResult doge = fetchOKXPrice(client, "DOGE-USDT");

    if (btc.price > 0) {
        cryptoData.btc = btc.price;   cryptoData.btcPct  = btc.pct24h;
        cryptoData.eth = eth.price;   cryptoData.ethPct  = eth.pct24h;
        cryptoData.sol = sol.price;   cryptoData.solPct  = sol.pct24h;
        cryptoData.doge = doge.price; cryptoData.dogePct = doge.pct24h;
        cryptoData.valid = true;
        Serial.printf("Crypto (OKX): BTC=$%.0f(%+.1f%%)  ETH=$%.1f(%+.1f%%)  SOL=$%.2f(%+.1f%%)  DOGE=$%.4f(%+.1f%%)\n",
                      cryptoData.btc, cryptoData.btcPct,
                      cryptoData.eth, cryptoData.ethPct,
                      cryptoData.sol, cryptoData.solPct,
                      cryptoData.doge, cryptoData.dogePct);
        return;
    }

    // --- 方案2: CryptoCompare (单次请求获取全部) ---
    Serial.println("OKX failed, trying CryptoCompare...");
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

    // --- 方案3: Binance ---
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

    // --- 方案4: CoinGecko ---
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

    // 获取 USD/CNY 24h 变化 (Yahoo Finance)
    if (fxData.valid && !fxData.hasPct) {
        Serial.println("Fetching USD/CNY 24h change...");
        http.begin(client, "https://query1.finance.yahoo.com/v8/finance/chart/USDCNY=X?interval=1d&range=2d");
        http.setTimeout(10000);
        http.addHeader("User-Agent", "Mozilla/5.0");
        code = http.GET();
        if (code == 200) {
            String payload = http.getString();
            JsonDocument doc2;
            if (!deserializeJson(doc2, payload)) {
                float prevClose = doc2["chart"]["result"][0]["meta"]["chartPreviousClose"].as<float>();
                if (prevClose > 0) {
                    fxData.pct24h = (fxData.usdcny - prevClose) / prevClose * 100.0f;
                    fxData.hasPct = true;
                    Serial.printf("FX 24h: %+.3f%%\n", fxData.pct24h);
                }
            }
        } else {
            Serial.printf("FX Yahoo failed: %d\n", code);
        }
        http.end();
    }
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

    // 方案1: Yahoo Finance (实时)
    http.begin(client, "https://query1.finance.yahoo.com/v8/finance/chart/GC=F?interval=1d&range=1d");
    http.setTimeout(10000);
    http.addHeader("User-Agent", "Mozilla/5.0");
    int code = http.GET();
    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            JsonObject meta = doc["chart"]["result"][0]["meta"];
            float p = meta["regularMarketPrice"].as<float>();
            float prevClose = meta["chartPreviousClose"].as<float>();
            if (p > 100) {
                goldData.priceUSD = p;
                goldData.pct24h = (prevClose > 0) ? (p - prevClose) / prevClose * 100.0f : 0;
                goldData.valid = true;
                Serial.printf("Gold (Yahoo): $%.1f/oz (%+.2f%%)\n", p, goldData.pct24h);
            }
        }
    } else {
        Serial.printf("  Yahoo Finance failed: %d\n", code);
    }
    http.end();
    if (goldData.valid) return;

    // 方案2: OKX PAXG-USDT (实时，黄金代币，误差<1%)
    http.begin(client, "https://www.okx.com/api/v5/market/ticker?instId=PAXG-USDT");
    http.setTimeout(10000);
    code = http.GET();
    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        if (!deserializeJson(doc, payload)) {
            float p = doc["data"][0]["last"].as<float>();
            if (p > 100) {
                goldData.priceUSD = p;
                goldData.valid = true;
                Serial.printf("Gold (PAXG): $%.1f/oz\n", p);
            }
        }
    } else {
        Serial.printf("  OKX PAXG failed: %d\n", code);
    }
    http.end();
    if (goldData.valid) return;

    // 方案3: jsDelivr CDN (每日更新，可能有延迟)
    http.begin(client, "https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/xau.min.json");
    http.setTimeout(10000);
    code = http.GET();
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
    display.print(wifiConnected ? "WiFi OK" : "WiFi --");
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
        y += 28;
        display.setFont(&FreeSans9pt7b);
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

// --- Right Panel: 加密货币 + 黄金 + 汇率 ---
void drawRightPanel()
{
    int x = L::MID_DIV + 14;
    int y = L::HDR_H + 28;
    char buf[48];
    int panelW = L::W - L::MID_DIV - 20;

    display.setTextColor(GxEPD_BLACK);

    // --- Crypto ---
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(x, y);
    display.print("Crypto");
    y += 10;
    display.drawFastHLine(x, y, panelW, GxEPD_BLACK);

    y += 30;
    int rightEdge = L::W - 16;
    if (cryptoData.valid) {
        struct { const char* sym; float price; float pct; } coins[] = {
            {"BTC",  cryptoData.btc,  cryptoData.btcPct},
            {"ETH",  cryptoData.eth,  cryptoData.ethPct},
            {"SOL",  cryptoData.sol,  cryptoData.solPct},
            {"DOGE", cryptoData.doge, cryptoData.dogePct},
        };
        int priceX = x + 82;
        for (auto &c : coins) {
            display.setFont(&FreeSans12pt7b);
            display.setCursor(x, y);
            display.print(c.sym);

            if (c.price >= 100000)
                snprintf(buf, sizeof(buf), "$%.0f", c.price);
            else if (c.price >= 1000)
                snprintf(buf, sizeof(buf), "$%.1f", c.price);
            else if (c.price >= 1)
                snprintf(buf, sizeof(buf), "$%.2f", c.price);
            else
                snprintf(buf, sizeof(buf), "$%.4f", c.price);
            display.setCursor(priceX, y);
            display.print(buf);

            // 24h 涨跌幅 (右对齐)
            display.setFont(&FreeSans9pt7b);
            char pctBuf[16];
            snprintf(pctBuf, sizeof(pctBuf), "%+.1f%%", c.pct);
            int16_t tx2, ty2; uint16_t tw2, th2;
            display.getTextBounds(pctBuf, 0, 0, &tx2, &ty2, &tw2, &th2);
            int pctX = rightEdge - tw2 - 14;
            display.setCursor(pctX, y);
            display.print(pctBuf);
            // 小箭头
            if (c.pct > 0.1f) drawArrowUp(rightEdge - 6, y - 6, 4);
            else if (c.pct < -0.1f) drawArrowDown(rightEdge - 6, y - 6, 4);

            y += 25;
        }
    } else {
        int priceX2 = x + 82;
        display.setFont(&FreeSans12pt7b);
        const char* names[] = {"BTC", "ETH", "SOL", "DOGE"};
        for (auto &n : names) {
            display.setCursor(x, y);
            display.print(n);
            display.setCursor(priceX2, y);
            display.print("------");
            y += 25;
        }
    }

    // --- Gold + FX ---
    y += 10;
    display.drawFastHLine(x, y, panelW, GxEPD_BLACK);
    y += 26;
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(x, y);
    display.print("Gold / FX");
    y += 10;
    display.drawFastHLine(x, y, panelW, GxEPD_BLACK);

    int gfxPriceX = x + 82;
    y += 30;
    // 金价 + 24h 涨跌
    display.setFont(&FreeSans12pt7b);
    display.setCursor(x, y);
    display.print("Gold");
    if (goldData.valid) {
        snprintf(buf, sizeof(buf), "$%.0f", goldData.priceUSD);
        display.setCursor(gfxPriceX, y);
        display.print(buf);

        display.setFont(&FreeSans9pt7b);
        char pctBuf[16];
        snprintf(pctBuf, sizeof(pctBuf), "%+.2f%%", goldData.pct24h);
        int16_t tx2, ty2; uint16_t tw2, th2;
        display.getTextBounds(pctBuf, 0, 0, &tx2, &ty2, &tw2, &th2);
        display.setCursor(rightEdge - tw2 - 14, y);
        display.print(pctBuf);
        if (goldData.pct24h > 0.01f) drawArrowUp(rightEdge - 6, y - 6, 4);
        else if (goldData.pct24h < -0.01f) drawArrowDown(rightEdge - 6, y - 6, 4);
    } else {
        display.setCursor(gfxPriceX, y);
        display.print("------");
    }

    y += 26;
    // 汇率 + 24h 涨跌
    display.setFont(&FreeSans12pt7b);
    display.setCursor(x, y);
    display.print("CNY");
    if (fxData.valid) {
        snprintf(buf, sizeof(buf), "%.4f", fxData.usdcny);
        display.setCursor(gfxPriceX, y);
        display.print(buf);

        if (fxData.hasPct) {
            display.setFont(&FreeSans9pt7b);
            char pctBuf[16];
            snprintf(pctBuf, sizeof(pctBuf), "%+.2f%%", fxData.pct24h);
            int16_t tx2, ty2; uint16_t tw2, th2;
            display.getTextBounds(pctBuf, 0, 0, &tx2, &ty2, &tw2, &th2);
            display.setCursor(rightEdge - tw2 - 14, y);
            display.print(pctBuf);
            if (fxData.pct24h > 0.01f) drawArrowUp(rightEdge - 6, y - 6, 4);
            else if (fxData.pct24h < -0.01f) drawArrowDown(rightEdge - 6, y - 6, 4);
        }
    } else {
        display.setCursor(gfxPriceX, y);
        display.print("------");
    }

    // 恐惧贪婪指数（数字加粗）
    if (fgData.valid) {
        y += 14;
        display.drawFastHLine(x, y, panelW, GxEPD_BLACK);
        y += 22;
        display.setFont(&FreeSansBold12pt7b);
        snprintf(buf, sizeof(buf), "F&G %d", fgData.value);
        display.setCursor(x, y);
        display.print(buf);
        int16_t tx2, ty2; uint16_t tw2, th2;
        display.getTextBounds(buf, x, y, &tx2, &ty2, &tw2, &th2);
        display.setFont(&FreeSans9pt7b);
        display.setCursor(x + tw2 + 6, y);
        display.print(fgData.label);
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

    // Uptime (右侧)
    unsigned long sec = millis() / 1000;
    snprintf(buf, sizeof(buf), "Up %luh%lum", sec / 3600, (sec % 3600) / 60);
    display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
    display.setCursor(L::W - tw - 14, textY);
    display.print(buf);
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
    Serial.println("\n=== E-Paper Desktop Monitor - Phase 5 ===");

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

    Serial.println("Phase 5 ready.");
}

void loop()
{
    unsigned long now = millis();

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
        goldData.valid = false;  // 强制刷新金价
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
