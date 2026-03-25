"""
Cursor Usage 本地代理服务器

在电脑上运行，为 ESP32 墨水屏提供 Cursor 用量数据。
ESP32 通过局域网调用 http://<电脑IP>:8765/api/usage 获取数据。

使用方法:
  1. 复制 .env.example 为 .env 并填入你的 Cookie
  2. pip install -r requirements.txt
  3. python cursor_server.py
"""

import os
import re
import json
import time
import threading
from flask import Flask, jsonify
from dotenv import load_dotenv

load_dotenv()

app = Flask(__name__)

CURSOR_COOKIE = os.getenv("CURSOR_COOKIE", "")
CURSOR_API_KEY = os.getenv("CURSOR_API_KEY", "")
CURSOR_TEAM_ID = os.getenv("CURSOR_TEAM_ID", "")
SERVER_PORT = int(os.getenv("SERVER_PORT", "8765"))

cached_usage = {
    "included_used": 0,
    "included_total": 500,
    "ondemand_used": 0.0,
    "ondemand_limit": 10.0,
    "reset_date": "",
    "last_updated": 0,
    "error": None
}

cached_gold = {
    "price_usd": 0.0,
    "last_updated": 0,
    "error": None
}

cache_lock = threading.Lock()

CACHE_TTL = 300  # 5 minutes


def fetch_usage_via_cookie():
    """通过 Cookie 获取 Cursor usage 数据"""
    import requests

    headers = {
        "Cookie": CURSOR_COOKIE,
        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/131.0.0.0",
        "Accept": "application/json, text/plain, */*",
        "Referer": "https://www.cursor.com/cn/dashboard/usage",
    }

    # 尝试常见的 API 端点
    endpoints = [
        "https://www.cursor.com/api/usage",
        "https://www.cursor.com/api/dashboard/usage",
        "https://www.cursor.com/api/auth/usage",
    ]

    for url in endpoints:
        try:
            resp = requests.get(url, headers=headers, timeout=15)
            if resp.status_code == 200:
                data = resp.json()
                print(f"[OK] Fetched from {url}: {json.dumps(data, indent=2)[:500]}")
                return parse_usage_response(data)
        except Exception as e:
            print(f"[WARN] {url}: {e}")
            continue

    # 如果 API 端点都不行，尝试抓取 HTML 页面
    try:
        resp = requests.get("https://www.cursor.com/cn/dashboard/usage",
                            headers=headers, timeout=15)
        if resp.status_code == 200:
            return parse_usage_html(resp.text)
    except Exception as e:
        print(f"[ERROR] HTML scrape failed: {e}")

    return None


def fetch_usage_via_api_key():
    """通过 Admin API Key 获取 usage 数据（Business/Enterprise）"""
    import requests

    headers = {
        "Authorization": f"Bearer {CURSOR_API_KEY}",
        "Content-Type": "application/json",
    }

    try:
        url = f"https://www.cursor.com/api/dashboard/teams/{CURSOR_TEAM_ID}/usage"
        resp = requests.get(url, headers=headers, timeout=15)
        if resp.status_code == 200:
            data = resp.json()
            print(f"[OK] API Key fetch: {json.dumps(data, indent=2)[:500]}")
            return parse_usage_response(data)
    except Exception as e:
        print(f"[ERROR] API Key fetch: {e}")

    return None


def parse_usage_response(data):
    """解析 API JSON 响应"""
    result = {}

    # 根据实际 API 返回格式解析
    # 常见字段名可能是: usageCount, limit, used, total, requests 等
    if isinstance(data, dict):
        # 尝试直接字段
        for key in ["included_used", "usageCount", "used", "requestCount", "requests_used"]:
            if key in data:
                result["included_used"] = int(data[key])
                break

        for key in ["included_total", "limit", "total", "requestLimit", "requests_limit"]:
            if key in data:
                result["included_total"] = int(data[key])
                break

        if "reset_date" in data:
            result["reset_date"] = data["reset_date"]

    return result if result else None


def parse_usage_html(html):
    """从 HTML 页面提取 usage 数据"""
    result = {}

    # 匹配 "102 / 500" 格式
    match = re.search(r'(\d+)\s*/\s*(\d+)', html)
    if match:
        result["included_used"] = int(match.group(1))
        result["included_total"] = int(match.group(2))

    # 匹配 "US$0 / US$10" 格式
    match_od = re.search(r'US\$(\d+(?:\.\d+)?)\s*/\s*US\$(\d+(?:\.\d+)?)', html)
    if match_od:
        result["ondemand_used"] = float(match_od.group(1))
        result["ondemand_limit"] = float(match_od.group(2))

    return result if result else None


def refresh_cache():
    """刷新缓存"""
    global cached_usage

    print("[INFO] Refreshing Cursor usage data...")

    result = None
    if CURSOR_API_KEY and CURSOR_TEAM_ID:
        result = fetch_usage_via_api_key()

    if not result and CURSOR_COOKIE:
        result = fetch_usage_via_cookie()

    with cache_lock:
        if result:
            cached_usage.update(result)
            cached_usage["last_updated"] = int(time.time())
            cached_usage["error"] = None
            print(f"[OK] Usage: {cached_usage['included_used']}/{cached_usage['included_total']}")
        else:
            cached_usage["error"] = "Failed to fetch usage data"
            cached_usage["last_updated"] = int(time.time())
            print("[ERROR] Failed to refresh usage data")


def refresh_gold():
    """刷新金价数据"""
    import requests

    gold_apis = [
        ("https://api.metals.live/v1/spot/gold", lambda d: d[0]["gold"]),
        ("https://data-asg.goldprice.org/dbXRates/USD", lambda d: d["items"][0]["xauPrice"]),
    ]

    for url, parser in gold_apis:
        try:
            resp = requests.get(url, timeout=15)
            if resp.status_code == 200:
                price = parser(resp.json())
                if price and price > 0:
                    with cache_lock:
                        cached_gold["price_usd"] = float(price)
                        cached_gold["last_updated"] = int(time.time())
                        cached_gold["error"] = None
                    print(f"[OK] Gold: ${price:.1f}/oz from {url}")
                    return
        except Exception as e:
            print(f"[WARN] Gold {url}: {e}")

    with cache_lock:
        cached_gold["error"] = "Failed to fetch gold price"
        cached_gold["last_updated"] = int(time.time())
    print("[ERROR] Failed to fetch gold price from all sources")


def background_refresh():
    """后台定时刷新"""
    while True:
        try:
            refresh_cache()
        except Exception as e:
            print(f"[ERROR] Background refresh (cursor): {e}")
        try:
            refresh_gold()
        except Exception as e:
            print(f"[ERROR] Background refresh (gold): {e}")
        time.sleep(CACHE_TTL)


# ============================================================
# API 端点
# ============================================================

@app.route("/api/usage")
def api_usage():
    with cache_lock:
        return jsonify(cached_usage)


@app.route("/api/gold")
def api_gold():
    with cache_lock:
        return jsonify(cached_gold)


@app.route("/api/health")
def api_health():
    return jsonify({"status": "ok", "timestamp": int(time.time())})


@app.route("/api/manual", methods=["POST"])
def api_manual_update():
    """手动更新 usage 数据（备用方案：用户直接 POST 数据）"""
    from flask import request
    data = request.get_json()
    if data:
        with cache_lock:
            if "included_used" in data:
                cached_usage["included_used"] = int(data["included_used"])
            if "included_total" in data:
                cached_usage["included_total"] = int(data["included_total"])
            cached_usage["last_updated"] = int(time.time())
            cached_usage["error"] = None
        return jsonify({"status": "updated"})
    return jsonify({"error": "no data"}), 400


# ============================================================
# 启动
# ============================================================

if __name__ == "__main__":
    if not CURSOR_COOKIE and not CURSOR_API_KEY:
        print("=" * 60)
        print("WARNING: 没有配置 Cookie 或 API Key!")
        print("请复制 .env.example 为 .env 并填入你的 Cursor Cookie")
        print("")
        print("获取方法:")
        print("  1. 浏览器打开 https://cursor.com/cn/dashboard/usage")
        print("  2. F12 -> Network -> 刷新页面")
        print("  3. 找 API 请求，复制 Cookie 头")
        print("")
        print("服务器仍然会启动，你可以通过 /api/manual 手动推送数据:")
        print(f'  curl -X POST http://localhost:{SERVER_PORT}/api/manual \\')
        print('    -H "Content-Type: application/json" \\')
        print('    -d \'{"included_used": 102, "included_total": 500}\'')
        print("=" * 60)
    else:
        # 启动后台刷新线程
        t = threading.Thread(target=background_refresh, daemon=True)
        t.start()

    print(f"\nCursor Usage 代理服务器启动在 http://0.0.0.0:{SERVER_PORT}")
    print(f"ESP32 请调用: http://<本机IP>:{SERVER_PORT}/api/usage\n")

    app.run(host="0.0.0.0", port=SERVER_PORT, debug=False)
