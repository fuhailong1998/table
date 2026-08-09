/**
 * 墨水屏桌面信息显示器
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include <time.h>
#include <GxEPD2_BW.h>
#include <Adafruit_BME280.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
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
    bool valid, hasPct;
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

bool parseMarketFloat(const char *text, float &value)
{
    if (!text || !text[0]) return false;

    char *end = nullptr;
    value = strtof(text, &end);
    if (end == text) return false;
    while (*end && isspace(static_cast<unsigned char>(*end))) end++;
    return *end == '\0' && isfinite(value);
}

bool fetchMarketPayload(WiFiClientSecure &client, const char *url,
                        const char *source, String &payload)
{
    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.printf("%s: HTTP begin failed\n", source);
        return false;
    }

    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    http.setReuse(false);
    int code = http.GET();

    bool ok = false;
    if (code == HTTP_CODE_OK) {
        payload = http.getString();
        ok = !payload.isEmpty();
        if (!ok) Serial.printf("%s: empty response\n", source);
    } else if (code > 0) {
        Serial.printf("%s failed: HTTP %d\n", source, code);
    } else {
        String error = HTTPClient::errorToString(code);
        Serial.printf("%s failed: %d (%s)\n", source, code, error.c_str());
    }

    http.end();
    return ok;
}

bool validCryptoPrices(const float prices[4])
{
    for (int i = 0; i < 4; i++) {
        if (!isfinite(prices[i]) || prices[i] <= 0) return false;
    }
    return true;
}

void fillCryptoData(CryptoData &out, const float prices[4],
                    const float changes[4], bool hasPct)
{
    out.btc = prices[0];
    out.eth = prices[1];
    out.sol = prices[2];
    out.doge = prices[3];
    out.btcPct = changes[0];
    out.ethPct = changes[1];
    out.solPct = changes[2];
    out.dogePct = changes[3];
    out.valid = true;
    out.hasPct = hasPct;
}

int cryptoSymbolIndex(const char *symbol)
{
    if (!symbol) return -1;
    if (strcmp(symbol, "BTCUSDT") == 0) return 0;
    if (strcmp(symbol, "ETHUSDT") == 0) return 1;
    if (strcmp(symbol, "SOLUSDT") == 0) return 2;
    if (strcmp(symbol, "DOGEUSDT") == 0) return 3;
    return -1;
}

bool fetchCryptoFromBinance(WiFiClientSecure &client, CryptoData &out)
{
    String payload;
    if (!fetchMarketPayload(client, CRYPTO_BINANCE_API,
                            "Binance Vision", payload)) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc.is<JsonArray>()) {
        Serial.printf("Binance Vision JSON invalid: %s\n",
                      error ? error.c_str() : "root is not an array");
        return false;
    }

    float prices[4] = {};
    float changes[4] = {};
    bool seen[4] = {};

    for (JsonObjectConst ticker : doc.as<JsonArrayConst>()) {
        int index = cryptoSymbolIndex(ticker["symbol"].as<const char *>());
        if (index < 0) continue;

        float last = 0;
        float open = 0;
        if (!parseMarketFloat(ticker["lastPrice"].as<const char *>(), last) ||
            !parseMarketFloat(ticker["openPrice"].as<const char *>(), open) ||
            last <= 0 || open <= 0) {
            continue;
        }

        float pct = (last - open) / open * 100.0f;
        if (!isfinite(pct) || fabsf(pct) > 1000.0f) continue;

        prices[index] = last;
        changes[index] = pct;
        seen[index] = true;
    }

    for (int i = 0; i < 4; i++) {
        if (!seen[i]) {
            Serial.println("Binance Vision: incomplete ticker set");
            return false;
        }
    }

    if (!validCryptoPrices(prices)) return false;
    fillCryptoData(out, prices, changes, true);
    return true;
}

bool fetchCryptoFromCoinGecko(WiFiClientSecure &client, CryptoData &out)
{
    String payload;
    if (!fetchMarketPayload(client, CRYPTO_COINGECKO_API,
                            "CoinGecko", payload)) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.printf("CoinGecko JSON invalid: %s\n", error.c_str());
        return false;
    }

    const char *ids[] = {"bitcoin", "ethereum", "solana", "dogecoin"};
    float prices[4] = {};
    float changes[4] = {};
    bool hasPct = true;

    for (int i = 0; i < 4; i++) {
        JsonVariantConst price = doc[ids[i]]["usd"];
        if (price.isNull()) return false;
        prices[i] = price.as<float>();

        JsonVariantConst pct = doc[ids[i]]["usd_24h_change"];
        if (pct.isNull()) {
            hasPct = false;
        } else {
            changes[i] = pct.as<float>();
            if (!isfinite(changes[i]) || fabsf(changes[i]) > 1000.0f) {
                hasPct = false;
            }
        }
    }

    if (!validCryptoPrices(prices)) {
        Serial.println("CoinGecko: incomplete price set");
        return false;
    }
    if (!hasPct) memset(changes, 0, sizeof(changes));
    fillCryptoData(out, prices, changes, hasPct);
    return true;
}

bool fetchCryptoFromCdn(WiFiClientSecure &client, CryptoData &out)
{
    String payload;
    if (!fetchMarketPayload(client, CRYPTO_CDN_API,
                            "Crypto CDN", payload)) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.printf("Crypto CDN JSON invalid: %s\n", error.c_str());
        return false;
    }

    const char *symbols[] = {"btc", "eth", "sol", "doge"};
    float prices[4] = {};
    float changes[4] = {};
    for (int i = 0; i < 4; i++) {
        JsonVariantConst rateValue = doc["usd"][symbols[i]];
        if (rateValue.isNull()) return false;
        float unitsPerUsd = rateValue.as<float>();
        if (!isfinite(unitsPerUsd) || unitsPerUsd <= 0) return false;
        prices[i] = 1.0f / unitsPerUsd;
    }

    if (!validCryptoPrices(prices)) {
        Serial.println("Crypto CDN: incomplete price set");
        return false;
    }
    fillCryptoData(out, prices, changes, false);
    return true;
}

void fetchCryptoPrices()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);

    Serial.println("Fetching crypto...");
    CryptoData candidate = {};
    const char *source = nullptr;
    if (fetchCryptoFromBinance(client, candidate)) {
        source = "Binance Vision";
    } else if (fetchCryptoFromCoinGecko(client, candidate)) {
        source = "CoinGecko";
    } else if (fetchCryptoFromCdn(client, candidate)) {
        source = "CDN daily";
    }

    if (!source) {
        Serial.println("Crypto: all sources failed; keeping cached data.");
        return;
    }

    cryptoData = candidate;
    if (cryptoData.hasPct) {
        Serial.printf("Crypto (%s): BTC=$%.0f(%+.1f%%)  ETH=$%.1f(%+.1f%%)  SOL=$%.2f(%+.1f%%)  DOGE=$%.4f(%+.1f%%)\n",
                      source, cryptoData.btc, cryptoData.btcPct,
                      cryptoData.eth, cryptoData.ethPct,
                      cryptoData.sol, cryptoData.solPct,
                      cryptoData.doge, cryptoData.dogePct);
    } else {
        Serial.printf("Crypto (%s): BTC=$%.0f  ETH=$%.1f  SOL=$%.2f  DOGE=$%.4f (price only)\n",
                      source, cryptoData.btc, cryptoData.eth,
                      cryptoData.sol, cryptoData.doge);
    }
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
// API: 股市指数 (腾讯简版行情)
// ============================================================

bool getTildeField(const String &record, int target, String &field)
{
    int fieldIndex = 0;
    int start = 0;
    for (int i = 0; i <= static_cast<int>(record.length()); i++) {
        if (i == static_cast<int>(record.length()) || record[i] == '~') {
            if (fieldIndex == target) {
                field = record.substring(start, i);
                return true;
            }
            fieldIndex++;
            start = i + 1;
        }
    }
    return false;
}

int tencentIndexForVariable(const String &variable)
{
    if (variable == "v_s_usINX") return 0;
    if (variable == "v_s_usIXIC") return 1;
    if (variable == "v_s_usDJI") return 2;
    if (variable == "v_s_sh000001") return 3;
    return -1;
}

bool parseTencentIndexLine(const String &line, int &index,
                           float &price, float &pctChange)
{
    int equals = line.indexOf('=');
    int firstQuote = line.indexOf('"', equals + 1);
    int lastQuote = line.lastIndexOf('"');
    if (equals <= 0 || firstQuote < 0 || lastQuote <= firstQuote) return false;

    index = tencentIndexForVariable(line.substring(0, equals));
    if (index < 0) return false;

    String record = line.substring(firstQuote + 1, lastQuote);
    const char *expectedCodes[] = {".INX", ".IXIC", ".DJI", "000001"};
    String codeMarker = "~";
    codeMarker += expectedCodes[index];
    codeMarker += "~";
    int valuesStart = record.indexOf(codeMarker);
    if (valuesStart < 0) return false;

    // 从 ASCII 指数代码后开始解析，完全跳过前面的 GBK 中文名称。
    String values = record.substring(valuesStart + codeMarker.length());
    String priceField;
    String changeField;
    String pctField;
    if (!getTildeField(values, 0, priceField) ||
        !getTildeField(values, 1, changeField) ||
        !getTildeField(values, 2, pctField)) {
        return false;
    }

    float change = 0;
    if (!parseMarketFloat(priceField.c_str(), price) ||
        !parseMarketFloat(changeField.c_str(), change) ||
        !parseMarketFloat(pctField.c_str(), pctChange) ||
        price <= 0 || fabsf(pctChange) > 100.0f) {
        return false;
    }

    float previousClose = price - change;
    if (previousClose <= 0) return false;
    float calculatedPct = change / previousClose * 100.0f;
    return isfinite(calculatedPct) && fabsf(calculatedPct - pctChange) <= 0.1f;
}

void fetchStockIndices()
{
    if (!wifiConnected) return;

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(10);

    Serial.println("Fetching stock indices...");
    String payload;
    if (!fetchMarketPayload(client, STOCK_INDEX_API,
                            "Tencent indices", payload)) {
        Serial.println("Stock indices: request failed; keeping cached data.");
        return;
    }

    float prices[4] = {};
    float changes[4] = {};
    bool fresh[4] = {};
    int lineStart = 0;
    while (lineStart < static_cast<int>(payload.length())) {
        int lineEnd = payload.indexOf('\n', lineStart);
        if (lineEnd < 0) lineEnd = payload.length();
        String line = payload.substring(lineStart, lineEnd);
        line.trim();

        int index = -1;
        float price = 0;
        float pctChange = 0;
        if (parseTencentIndexLine(line, index, price, pctChange)) {
            prices[index] = price;
            changes[index] = pctChange;
            fresh[index] = true;
        }
        lineStart = lineEnd + 1;
    }

    int freshCount = 0;
    for (int i = 0; i < 4; i++) {
        if (!fresh[i]) continue;
        indexData.items[i].price = prices[i];
        indexData.items[i].pctChange = changes[i];
        indexData.items[i].valid = true;
        freshCount++;
    }

    indexData.valid = false;
    for (int i = 0; i < 4; i++) {
        indexData.valid = indexData.valid || indexData.items[i].valid;
    }

    if (freshCount == 0) {
        Serial.println("Stock indices: invalid response; keeping cached data.");
        return;
    }

    Serial.printf("Stock indices (Tencent): %d/4 updated\n", freshCount);
    for (int i = 0; i < 4; i++) {
        if (indexData.items[i].valid) {
            Serial.printf("  %s: %.1f (%+.2f%%)%s\n",
                          indexData.items[i].name,
                          indexData.items[i].price,
                          indexData.items[i].pctChange,
                          fresh[i] ? "" : " [cached]");
        }
    }
}

// ============================================================
// UI 布局 (648 x 480)
//
//  +----------------------------------------------------------+
//  | WiFi / weekday   YYYY.MM.DD                 HH:MM | 0-87  |
//  |-----------------------+----------------------------------|
//  | Outdoor / Indoor      | Markets                  | 88-423|
//  |-----------------------+----------------------------------|
//  | Presence     Distance | Today at desk            |424-479|
//  +----------------------------------------------------------+
//
// 所有局部刷新窗口均按 8px 对齐，避免 GxEPD2 自动扩展到相邻区域。
// ============================================================

namespace L {
    const int16_t M       = 16;
    const int16_t HDR_H   = 88;
    const int16_t MID_DIV = 288;
    const int16_t FOOT_Y  = 424;
    const int16_t W       = EPD_WIDTH;
    const int16_t H       = EPD_HEIGHT;
}

void drawTextRight(const char *text, int16_t rightX, int16_t baseline)
{
    int16_t tx, ty;
    uint16_t tw, th;
    display.getTextBounds(text, 0, 0, &tx, &ty, &tw, &th);
    display.setCursor(rightX - tw - tx, baseline);
    display.print(text);
}

void drawTextCentered(const char *text, int16_t leftX, int16_t rightX, int16_t baseline)
{
    int16_t tx, ty;
    uint16_t tw, th;
    display.getTextBounds(text, 0, 0, &tx, &ty, &tw, &th);
    display.setCursor(leftX + (rightX - leftX - tw) / 2 - tx, baseline);
    display.print(text);
}

void drawSectionLabel(const char *label, int16_t x, int16_t rightX, int16_t baseline)
{
    int16_t tx, ty;
    uint16_t tw, th;
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(x, baseline);
    display.print(label);
    display.getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);
    int16_t lineX = x + tw + 12;
    if (lineX < rightX) {
        display.drawFastHLine(lineX, baseline - 6, rightX - lineX, GxEPD_BLACK);
    }
}

void drawWifiStatus(int16_t x, int16_t baseline)
{
    bool connected = wifiConnected && WiFi.status() == WL_CONNECTED;
    int bars = 0;
    if (connected) {
        int rssi = WiFi.RSSI();
        bars = (rssi >= -55) ? 4 : (rssi >= -65) ? 3 : (rssi >= -75) ? 2 : 1;
    }

    int16_t bottom = baseline - 2;
    for (int i = 0; i < 4; i++) {
        int16_t h = 3 + i * 3;
        int16_t bx = x + i * 5;
        display.drawRect(bx, bottom - h, 3, h, GxEPD_BLACK);
        if (i < bars) {
            display.fillRect(bx, bottom - h, 3, h, GxEPD_BLACK);
        }
    }

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(x + 26, baseline);
    display.print(connected ? "ONLINE" : "OFFLINE");
}

// --- Header: 联网状态 + 日期 + 大时间 ---
void drawTimeArea()
{
    char dateBuf[24];
    char timeBuf[12];
    if (timeData.valid) {
        snprintf(dateBuf, sizeof(dateBuf), "%04d.%02d.%02d",
                 timeData.year, timeData.month, timeData.day);
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d",
                 timeData.hour, timeData.minute);
    } else {
        strcpy(dateBuf, "----.--.--");
        strcpy(timeBuf, "--:--");
    }

    display.setTextSize(1);
    display.setTextColor(GxEPD_BLACK);
    drawWifiStatus(L::M, 27);

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(151, 27);
    display.print(timeData.valid ? weekdayName(timeData.weekday) : "---");

    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(L::M, 68);
    display.print(dateBuf);

    display.setFont(&FreeSansBold24pt7b);
    drawTextRight(timeBuf, L::W - L::M, 68);

    display.drawFastHLine(0, L::HDR_H - 2, L::W, GxEPD_BLACK);
    display.drawFastHLine(0, L::HDR_H - 1, L::W, GxEPD_BLACK);
}

// 趋势图形 (cx, cy 为中心，sz 为半径)
void drawArrowUp(int cx, int cy, int sz)
{
    display.fillTriangle(cx, cy - sz, cx - sz, cy + sz,
                         cx + sz, cy + sz, GxEPD_BLACK);
}

void drawArrowDown(int cx, int cy, int sz)
{
    display.fillTriangle(cx, cy + sz, cx - sz, cy - sz,
                         cx + sz, cy - sz, GxEPD_BLACK);
}

void drawArrowFlat(int cx, int cy, int sz)
{
    display.drawFastHLine(cx - sz, cy, sz * 2, GxEPD_BLACK);
    display.drawFastHLine(cx - sz, cy - 1, sz * 2, GxEPD_BLACK);
}

const char *aqiStatusLabel(int aqi)
{
    if (aqi <= 50) return "GOOD";
    if (aqi <= 100) return "FAIR";
    if (aqi <= 150) return "POOR";
    return "BAD";
}

const char *comfortStatusLabel(float temperature, float humidity)
{
    int tScore = (temperature >= 18 && temperature <= 26) ? 0
               : (temperature >= 10 && temperature <= 30) ? 1 : 2;
    int hScore = (humidity >= 30 && humidity <= 60) ? 0
               : (humidity >= 20 && humidity <= 80) ? 1 : 2;
    int score = tScore + hScore;
    if (score == 0) return "GOOD";
    if (score <= 2) return "FAIR";
    return "POOR";
}

void drawForecastRow(const char *label, int weatherCode, float low, float high,
                     bool valid, int16_t baseline)
{
    char rangeBuf[24];
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(L::M, baseline);
    display.print(label);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(104, baseline);
    display.print(valid ? weatherCodeToStr(weatherCode) : "NO DATA");

    if (valid) {
        snprintf(rangeBuf, sizeof(rangeBuf), "%.0f / %.0f", low, high);
    } else {
        strcpy(rangeBuf, "-- / --");
    }
    display.setFont(&FreeMonoBold9pt7b);
    drawTextRight(rangeBuf, L::MID_DIV - L::M, baseline);
}

// --- Left Panel: 户外天气 + 室内环境 ---
void drawLeftPanel()
{
    const int16_t rightX = L::MID_DIV - L::M;
    char buf[40];
    int16_t tx, ty;
    uint16_t tw, th;

    display.setTextSize(1);
    display.setTextColor(GxEPD_BLACK);
    drawSectionLabel("OUTDOOR", L::M, rightX, 113);

    if (weatherData.valid) {
        snprintf(buf, sizeof(buf), "%.0f", weatherData.currentTemp);
        display.setFont(&FreeSansBold24pt7b);
        display.setCursor(L::M, 185);
        display.print(buf);
        display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
        display.drawCircle(L::M + tw + 5, 153, 3, GxEPD_BLACK);
        display.setFont(&FreeSansBold12pt7b);
        display.setCursor(L::M + tw + 15, 181);
        display.print("C");

        display.setFont(&FreeSansBold12pt7b);
        display.setCursor(132, 153);
        display.print(weatherCodeToStr(weatherData.currentCode));
    } else {
        display.setFont(&FreeSansBold24pt7b);
        display.setCursor(L::M, 185);
        display.print("--");
        display.setFont(&FreeSansBold12pt7b);
        display.setCursor(132, 153);
        display.print("NO DATA");
    }

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(132, 178);
    if (aqiData.valid) {
        snprintf(buf, sizeof(buf), "AQI %d", aqiData.aqi);
        display.print(buf);
        drawTextRight(aqiStatusLabel(aqiData.aqi), rightX, 178);
        display.setFont(&FreeSans9pt7b);
        display.setCursor(132, 200);
        display.printf("PM2.5  %.0f", aqiData.pm25);
    } else {
        display.print("AQI --");
        drawTextRight("NO DATA", rightX, 178);
        display.setFont(&FreeSans9pt7b);
        display.setCursor(132, 200);
        display.print("PM2.5  --");
    }

    drawForecastRow("TODAY", weatherData.todayCode, weatherData.todayLow,
                    weatherData.todayHigh, weatherData.valid, 226);
    drawForecastRow("TMRW", weatherData.tmrwCode, weatherData.tmrwLow,
                    weatherData.tmrwHigh, weatherData.valid, 249);

    drawSectionLabel("INDOOR", L::M, rightX, 275);

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(L::M, 301);
    display.print("TEMP");
    display.setCursor(150, 301);
    display.print("HUMIDITY");

    if (sensorData.valid && sensorData.hasPrev) {
        float tempDiff = sensorData.temperature - sensorData.prevTemp;
        if (tempDiff > 0.2f) drawArrowUp(75, 295, 4);
        else if (tempDiff < -0.2f) drawArrowDown(75, 295, 4);
        else drawArrowFlat(75, 295, 4);

        float humidDiff = sensorData.humidity - sensorData.prevHumid;
        if (humidDiff > 0.5f) drawArrowUp(258, 295, 4);
        else if (humidDiff < -0.5f) drawArrowDown(258, 295, 4);
        else drawArrowFlat(258, 295, 4);
    }

    display.setFont(&FreeSansBold18pt7b);
    if (sensorData.valid) {
        snprintf(buf, sizeof(buf), "%.1f", sensorData.temperature);
        display.setCursor(L::M, 344);
        display.print(buf);
        display.getTextBounds(buf, 0, 0, &tx, &ty, &tw, &th);
        display.drawCircle(L::M + tw + 4, 320, 3, GxEPD_BLACK);
        display.setFont(&FreeSansBold9pt7b);
        display.setCursor(L::M + tw + 14, 343);
        display.print("C");

        display.setFont(&FreeSansBold18pt7b);
        snprintf(buf, sizeof(buf), "%.0f%%", sensorData.humidity);
        display.setCursor(150, 344);
        display.print(buf);
    } else {
        display.setCursor(L::M, 344);
        display.print("--");
        display.setCursor(150, 344);
        display.print("--%");
    }

    display.drawFastHLine(L::M, 357, rightX - L::M, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(L::M, 378);
    display.print("PRESSURE");
    display.setCursor(150, 378);
    display.print("COMFORT");

    display.setFont(&FreeSansBold12pt7b);
    if (sensorData.valid) {
        snprintf(buf, sizeof(buf), "%.0f hPa", sensorData.pressure);
        display.setCursor(L::M, 410);
        display.print(buf);
        display.setCursor(150, 410);
        display.print(comfortStatusLabel(sensorData.temperature, sensorData.humidity));
    } else {
        display.setCursor(L::M, 410);
        display.print("---- hPa");
        display.setCursor(150, 410);
        display.print("NO DATA");
    }
}

void drawMarketRow(const char *name, const char *price, float pct,
                   bool hasPct, int16_t baseline)
{
    const int16_t x = L::MID_DIV + L::M;
    const int16_t priceRight = 520;
    const int16_t changeRight = L::W - L::M;

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(x, baseline);
    display.print(name);

    display.setFont(&FreeMonoBold9pt7b);
    drawTextRight(price, priceRight, baseline);

    if (hasPct) {
        char pctBuf[16];
        if (pct >= 1000.0f || pct <= -1000.0f) {
            snprintf(pctBuf, sizeof(pctBuf), "%+.0f%%", pct);
        } else {
            snprintf(pctBuf, sizeof(pctBuf), "%+.1f%%", pct);
        }
        if (pct > 0.05f) drawArrowUp(540, baseline - 6, 3);
        else if (pct < -0.05f) drawArrowDown(540, baseline - 6, 3);
        else drawArrowFlat(540, baseline - 6, 3);
        display.setFont(&FreeMonoBold9pt7b);
        drawTextRight(pctBuf, changeRight, baseline);
    }
}

// --- Right Panel: 固定行高的行情表 ---
void drawRightPanel()
{
    const int16_t x = L::MID_DIV + L::M;
    const int16_t rightX = L::W - L::M;
    char buf[40];

    display.setTextSize(1);
    display.setTextColor(GxEPD_BLACK);
    display.drawFastVLine(L::MID_DIV, L::HDR_H,
                          L::FOOT_Y - L::HDR_H, GxEPD_BLACK);
    drawSectionLabel("MARKETS", x, rightX, 113);

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(x, 137);
    display.print("ASSET");
    drawTextRight("PRICE", 520, 137);
    drawTextRight("24H", rightX, 137);

    const char *coinNames[] = {"BTC", "ETH", "SOL", "DOGE"};
    float coinPrices[] = {cryptoData.btc, cryptoData.eth,
                          cryptoData.sol, cryptoData.doge};
    float coinChanges[] = {cryptoData.btcPct, cryptoData.ethPct,
                           cryptoData.solPct, cryptoData.dogePct};
    const int16_t coinRows[] = {159, 181, 203, 225};
    for (int i = 0; i < 4; i++) {
        if (cryptoData.valid && coinPrices[i] > 0) {
            if (coinPrices[i] >= 100000) snprintf(buf, sizeof(buf), "$%.0f", coinPrices[i]);
            else if (coinPrices[i] >= 1000) snprintf(buf, sizeof(buf), "$%.1f", coinPrices[i]);
            else if (coinPrices[i] >= 1) snprintf(buf, sizeof(buf), "$%.2f", coinPrices[i]);
            else snprintf(buf, sizeof(buf), "$%.4f", coinPrices[i]);
            drawMarketRow(coinNames[i], buf, coinChanges[i],
                          cryptoData.hasPct, coinRows[i]);
        } else {
            drawMarketRow(coinNames[i], "--", 0, false, coinRows[i]);
        }
    }

    display.drawFastHLine(x, 236, rightX - x, GxEPD_BLACK);

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(x, 258);
    display.print("F&G");
    display.setFont(&FreeMonoBold9pt7b);
    if (fgData.valid) {
        snprintf(buf, sizeof(buf), "%d", fgData.value);
        drawTextRight(buf, 500, 258);
        display.setFont(&FreeSans9pt7b);
        drawTextRight(fgData.label, rightX, 258);
    } else {
        drawTextRight("--", 500, 258);
        display.setFont(&FreeSans9pt7b);
        drawTextRight("NO DATA", rightX, 258);
    }

    if (goldData.valid) snprintf(buf, sizeof(buf), "$%.0f", goldData.priceUSD);
    else strcpy(buf, "--");
    drawMarketRow("GOLD", buf, 0, false, 280);

    if (fxData.valid) snprintf(buf, sizeof(buf), "%.4f", fxData.usdcny);
    else strcpy(buf, "--");
    drawMarketRow("USD/CNY", buf, 0, false, 302);

    display.drawFastHLine(x, 312, rightX - x, GxEPD_BLACK);
    drawSectionLabel("INDICES", x, rightX, 331);

    const int16_t indexRows[] = {350, 371, 392, 413};
    for (int i = 0; i < 4; i++) {
        IndexItem &item = indexData.items[i];
        if (item.valid) {
            if (item.price >= 10000) snprintf(buf, sizeof(buf), "%.0f", item.price);
            else snprintf(buf, sizeof(buf), "%.1f", item.price);
            drawMarketRow(item.name, buf, item.pctChange, true, indexRows[i]);
        } else {
            drawMarketRow(item.name, "--", 0, false, indexRows[i]);
        }
    }
}

// --- Footer: 雷达状态、距离与今日在座时长 ---
void drawFooter()
{
    char buf[32];
    display.setTextSize(1);
    display.setTextColor(GxEPD_BLACK);
    display.drawFastHLine(0, L::FOOT_Y, L::W, GxEPD_BLACK);
    display.drawFastHLine(0, L::FOOT_Y + 1, L::W, GxEPD_BLACK);
    display.drawFastVLine(184, L::FOOT_Y + 12, 36, GxEPD_BLACK);
    display.drawFastVLine(360, L::FOOT_Y + 12, 36, GxEPD_BLACK);

    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(L::M, 447);
    display.print("PRESENCE");
    display.setCursor(204, 447);
    display.print("DISTANCE");
    display.setCursor(380, 447);
    display.print("TODAY AT DESK");

    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(L::M, 471);
    if (radarBytesTotal == 0) display.print("RADAR --");
    else if (!radarData.present) display.print("AWAY");
    else display.print(radarData.status == 2 ? "SEATED" : "MOVING");

    display.setCursor(204, 471);
    if (radarData.present && radarData.distanceCm > 0) {
        display.printf("%.1f m", radarData.distanceCm / 100.0f);
    } else {
        display.print("--");
    }

    unsigned long secs = radarData.todayOccupied / 1000;
    int hours = secs / 3600;
    int minutes = (secs % 3600) / 60;
    snprintf(buf, sizeof(buf), "%dh %02dm", hours, minutes);
    display.setCursor(380, 471);
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
        drawTimeArea();
    } while (display.nextPage());
}

void drawLeftPartial()
{
    display.setPartialWindow(0, L::HDR_H, L::MID_DIV,
                             L::FOOT_Y - L::HDR_H);
    display.firstPage();
    do {
        display.fillRect(0, L::HDR_H, L::MID_DIV,
                         L::FOOT_Y - L::HDR_H, GxEPD_WHITE);
        drawLeftPanel();
    } while (display.nextPage());
}

void drawRightPartial()
{
    display.setPartialWindow(L::MID_DIV, L::HDR_H,
                             L::W - L::MID_DIV, L::FOOT_Y - L::HDR_H);
    display.firstPage();
    do {
        display.fillRect(L::MID_DIV, L::HDR_H,
                         L::W - L::MID_DIV, L::FOOT_Y - L::HDR_H,
                         GxEPD_WHITE);
        drawRightPanel();
    } while (display.nextPage());
}

void drawFooterPartial()
{
    display.setPartialWindow(0, L::FOOT_Y, L::W, L::H - L::FOOT_Y);
    display.firstPage();
    do {
        display.fillRect(0, L::FOOT_Y, L::W, L::H - L::FOOT_Y,
                         GxEPD_WHITE);
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
    Serial.println("\n=== E-Paper Desktop Monitor ===");

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
    lastIndexFetch   = t;

    Serial.println("Monitor ready.");
}

void drawClockScreen()
{
    updateTimeData();
    display.setRotation(EPD_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);
        display.setTextSize(1);

        display.setFont(&FreeSansBold9pt7b);
        drawTextCentered("AWAY MODE", 0, L::W, 37);
        display.drawFastHLine(L::M, 55, L::W - L::M * 2, GxEPD_BLACK);

        char timeBuf[12];
        char dateBuf[32];
        if (timeData.valid) {
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d",
                     timeData.hour, timeData.minute);
            snprintf(dateBuf, sizeof(dateBuf), "%04d.%02d.%02d  /  %s",
                     timeData.year, timeData.month, timeData.day,
                     weekdayName(timeData.weekday));
        } else {
            strcpy(timeBuf, "--:--");
            strcpy(dateBuf, "----.--.--  /  ---");
        }

        display.setFont(&FreeSansBold24pt7b);
        display.setTextSize(2);
        drawTextCentered(timeBuf, 0, L::W, 260);

        display.setTextSize(1);
        display.setFont(&FreeSansBold12pt7b);
        drawTextCentered(dateBuf, 0, L::W, 326);

        char sensorBuf[40];
        if (sensorData.valid) {
            snprintf(sensorBuf, sizeof(sensorBuf), "INDOOR  %.1f C  /  %.0f%%",
                     sensorData.temperature, sensorData.humidity);
        } else {
            strcpy(sensorBuf, "INDOOR  NO DATA");
        }
        display.setFont(&FreeSans12pt7b);
        drawTextCentered(sensorBuf, 0, L::W, 369);

        display.drawFastHLine(0, L::FOOT_Y, L::W, GxEPD_BLACK);
        display.drawFastHLine(0, L::FOOT_Y + 1, L::W, GxEPD_BLACK);
        display.setFont(&FreeSansBold9pt7b);
        drawTextCentered("TODAY AT DESK", 0, L::W, 447);

        unsigned long secs = radarData.todayOccupied / 1000;
        int hours = secs / 3600;
        int minutes = (secs % 3600) / 60;
        snprintf(sensorBuf, sizeof(sensorBuf), "%dh %02dm", hours, minutes);
        display.setFont(&FreeSansBold12pt7b);
        drawTextCentered(sensorBuf, 0, L::W, 472);
    } while (display.nextPage());
    display.setTextSize(1);
}

void loop()
{
    unsigned long now = millis();
    bool wasAway = radarData.awayMode;

    pollRadar();
    updateOccupancy();

    // 达到离开阈值后立即切换，避免复用分钟计时器造成额外等待。
    if (!wasAway && radarData.awayMode) {
        updateTimeData();
        readSensor();
        display.init(115200, true, 50, false);
        drawClockScreen();
        lastTimeRefresh = now;
        lastSensorRead = now;
    }

    // 从离开模式回到正常模式 → 全刷
    if (wasAway && !radarData.awayMode) {
        updateTimeData();
        display.init(115200, true, 50, false);
        drawFullScreen();
        lastFullRefresh = now;
        lastTimeRefresh = now;
        delay(1000);
        return;
    }

    // 离开模式：只显示简洁时钟，降低刷新频率
    if (radarData.awayMode) {
        if (now - lastTimeRefresh >= 60000) {
            updateTimeData();
            readSensor();
            display.init(115200, false, 50, false);
            drawClockScreen();
            lastTimeRefresh = now;
            lastSensorRead = now;
        }
        delay(1000);
        return;
    }

    // === 正常模式 ===

    // 全刷优先于普通轮询，避免同一轮先局刷多次、随后又立即全刷。
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
        fetchStockIndices();
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
        lastIndexFetch   = now;
        delay(1000);
        return;
    }

    bool refreshHandled = false;

    // 每分钟：刷新时间 + Footer
    if (now - lastTimeRefresh >= 60000) {
        updateTimeData();
        display.init(115200, false, 50, false);
        drawTimePartial();
        drawFooterPartial();
        lastTimeRefresh = now;
        refreshHandled = true;
    }

    // 每 2 分钟：读传感器 + 局部刷新左面板
    if (!refreshHandled && now - lastSensorRead >= SENSOR_READ_INTERVAL) {
        readSensor();
        display.init(115200, false, 50, false);
        drawLeftPartial();
        lastSensorRead = now;
        refreshHandled = true;
    }

    // 每 2 分钟：获取加密货币 + 局部刷新右面板
    if (!refreshHandled && now - lastCryptoFetch >= CRYPTO_FETCH_INTERVAL) {
        ensureWiFi();
        fetchCryptoPrices();
        display.init(115200, false, 50, false);
        drawRightPartial();
        lastCryptoFetch = now;
        refreshHandled = true;
    }

    // 每 10 分钟：获取汇率 + 金价
    if (!refreshHandled && now - lastFxFetch >= EXCHANGE_FETCH_INTERVAL) {
        ensureWiFi();
        goldData.valid = false;
        fetchExchangeRate();
        fetchGoldPrice();
        display.init(115200, false, 50, false);
        drawRightPartial();
        lastFxFetch = now;
        refreshHandled = true;
    }

    if (!refreshHandled && now - lastWeatherFetch >= WEATHER_FETCH_INTERVAL) {
        ensureWiFi();
        fetchWeather();
        display.init(115200, false, 50, false);
        drawLeftPartial();
        lastWeatherFetch = now;
        refreshHandled = true;
    }

    // 每 10 分钟：恐惧贪婪 + AQI
    if (!refreshHandled && now - lastFgFetch >= FG_FETCH_INTERVAL) {
        ensureWiFi();
        fetchFearGreed();
        fetchAqi();
        display.init(115200, false, 50, false);
        drawLeftPartial();
        drawRightPartial();
        lastFgFetch = now;
        lastAqiFetch = now;
        refreshHandled = true;
    }

    // 每 10 分钟：获取股指
    if (!refreshHandled && now - lastIndexFetch >= INDEX_FETCH_INTERVAL) {
        ensureWiFi();
        fetchStockIndices();
        display.init(115200, false, 50, false);
        drawRightPartial();
        lastIndexFetch = now;
        refreshHandled = true;
    }

    delay(1000);
}
