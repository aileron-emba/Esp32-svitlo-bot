#!/usr/bin/env python3
"""Сервер «Світло-бот».

ESP32 шле пінги на POST /api/ping. Якщо пінгів нема довше за OFFLINE_TIMEOUT —
вважаємо, що світло зникло, і шлемо повідомлення в Telegram. Коли пінги
відновились — світло з'явилось.

Тільки стандартна бібліотека Python 3.9+, без залежностей.

Змінні оточення:
  BOT_TOKEN        токен Telegram-бота від @BotFather (обов'язково)
  API_KEY          спільний ключ, такий самий як у firmware/include/config.h
  PORT             порт HTTP (8080)
  OFFLINE_TIMEOUT  секунд без пінгів, після яких світла «нема» (90)
  DB_PATH          файл бази (svitlo.db)
  TIMEZONE         часовий пояс для повідомлень (Europe/Kyiv)
"""

import json
import logging
import os
import queue
import sqlite3
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from zoneinfo import ZoneInfo


def load_env_file(path):
    if not os.path.exists(path):
        return
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


load_env_file(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))

BOT_TOKEN = os.environ.get("BOT_TOKEN", "")
API_KEY = os.environ.get("API_KEY", "change-me")
PORT = int(os.environ.get("PORT", "8080"))
OFFLINE_TIMEOUT = int(os.environ.get("OFFLINE_TIMEOUT", "90"))
DB_PATH = os.environ.get("DB_PATH", "svitlo.db")
TZ = ZoneInfo(os.environ.get("TIMEZONE", "Europe/Kyiv"))

STARTED_AT = time.time()

log = logging.getLogger("svitlo")


# ---------- база ----------

class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.lock = threading.Lock()
        with self.lock, self.db:
            self.db.execute(
                """CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    chat_id TEXT NOT NULL,
                    name TEXT NOT NULL DEFAULT '',
                    last_seen REAL NOT NULL,
                    state TEXT NOT NULL,          -- 'on' | 'off'
                    changed_at REAL NOT NULL
                )"""
            )

    def get(self, device_id):
        with self.lock:
            return self.db.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()

    def by_chat(self, chat_id):
        with self.lock:
            return self.db.execute("SELECT * FROM devices WHERE chat_id=? ORDER BY name", (chat_id,)).fetchall()

    def online(self):
        with self.lock:
            return self.db.execute("SELECT * FROM devices WHERE state='on'").fetchall()

    def upsert(self, device_id, chat_id, name, last_seen, state, changed_at):
        with self.lock, self.db:
            self.db.execute(
                """INSERT INTO devices (device_id, chat_id, name, last_seen, state, changed_at)
                   VALUES (?, ?, ?, ?, ?, ?)
                   ON CONFLICT(device_id) DO UPDATE SET chat_id=excluded.chat_id, name=excluded.name,
                   last_seen=excluded.last_seen, state=excluded.state, changed_at=excluded.changed_at""",
                (device_id, chat_id, name, last_seen, state, changed_at),
            )

    def set_off(self, device_id, last_seen):
        """Позначити «нема світла», якщо з last_seen не було нових пінгів. True — стан змінився."""
        with self.lock, self.db:
            cur = self.db.execute(
                "UPDATE devices SET state='off', changed_at=last_seen "
                "WHERE device_id=? AND state='on' AND last_seen=?",
                (device_id, last_seen),
            )
            return cur.rowcount == 1


# ---------- Telegram ----------

def tg_call(method, params, timeout=15):
    url = f"https://api.telegram.org/bot{BOT_TOKEN}/{method}"
    data = json.dumps(params).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


class Outbox:
    """Черга повідомлень з повторними спробами: якщо інтернет на сервері моргнув — не губимо."""

    def __init__(self):
        self.q = queue.Queue()
        threading.Thread(target=self._worker, daemon=True).start()

    def send(self, chat_id, text):
        self.q.put((chat_id, text))

    def _worker(self):
        while True:
            chat_id, text = self.q.get()
            delay = 2
            for attempt in range(10):
                try:
                    tg_call("sendMessage", {"chat_id": chat_id, "text": text, "parse_mode": "HTML"})
                    log.info("-> %s: %s", chat_id, text.replace("\n", " | "))
                    break
                except urllib.error.HTTPError as e:
                    body = e.read().decode(errors="replace")
                    log.warning("Telegram %s для %s: %s", e.code, chat_id, body)
                    if 400 <= e.code < 500 and e.code != 429:
                        break  # неправильний chat_id і т.п. — повтор не допоможе
                except Exception as e:  # мережа
                    log.warning("Telegram недоступний (%s), спроба %d", e, attempt + 1)
                time.sleep(delay)
                delay = min(delay * 2, 60)


# ---------- форматування ----------

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def fmt_time(ts):
    return datetime.fromtimestamp(ts, TZ).strftime("%H:%M")


def fmt_duration(seconds):
    seconds = int(seconds)
    days, rem = divmod(seconds, 86400)
    hours, rem = divmod(rem, 3600)
    minutes = rem // 60
    parts = []
    if days:
        parts.append(f"{days} д")
    if hours:
        parts.append(f"{hours} год")
    if minutes or not parts:
        parts.append(f"{minutes} хв")
    return " ".join(parts)


def title(name):
    return f"<b>{esc(name)}</b>: " if name else ""


# ---------- логіка ----------

class Monitor:
    def __init__(self, store, outbox):
        self.store = store
        self.outbox = outbox
        self.lock = threading.Lock()  # пінг і перевірка таймауту не мають перетинатись
        threading.Thread(target=self._watch, daemon=True).start()

    def ping(self, device_id, chat_id, name):
        with self.lock:
            self._ping(device_id, chat_id, name)

    def _ping(self, device_id, chat_id, name):
        now = time.time()
        dev = self.store.get(device_id)
        if dev is None:
            self.store.upsert(device_id, chat_id, name, now, "on", now)
            self.outbox.send(chat_id, f"✅ {title(name)}Пристрій підключено. Світло є, стежу 👀")
            return
        if dev["chat_id"] != chat_id:
            self.outbox.send(chat_id, f"✅ {title(name)}Сповіщення тепер надходитимуть у цей чат.")
        if dev["state"] == "off":
            self.store.upsert(device_id, chat_id, name, now, "on", now)
            self.outbox.send(
                chat_id,
                f"💡 {title(name)}Світло з'явилось о {fmt_time(now)}\n"
                f"Не було {fmt_duration(now - dev['changed_at'])}",
            )
        else:
            self.store.upsert(device_id, chat_id, name, now, "on", dev["changed_at"])

    def _watch(self):
        while True:
            time.sleep(5)
            now = time.time()
            for dev in self.store.online():
                # Після перезапуску сервера даємо пристроям час знову вийти на зв'язок.
                last = max(dev["last_seen"], STARTED_AT)
                if now - last <= OFFLINE_TIMEOUT:
                    continue
                off_at = dev["last_seen"]
                with self.lock:
                    changed = self.store.set_off(dev["device_id"], off_at)
                if changed:
                    self.outbox.send(
                        dev["chat_id"],
                        f"🔴 {title(dev['name'])}Світло зникло о {fmt_time(off_at)}\n"
                        f"Було {fmt_duration(off_at - dev['changed_at'])}",
                    )


def status_text(chat_id, store):
    devices = store.by_chat(str(chat_id))
    if not devices:
        return "До цього чату ще не підключено жодного пристрою."
    now = time.time()
    lines = []
    for d in devices:
        dur = fmt_duration(now - d["changed_at"])
        name = esc(d["name"] or d["device_id"])
        if d["state"] == "on":
            lines.append(f"💡 <b>{name}</b>: світло є вже {dur} (з {fmt_time(d['changed_at'])})")
        else:
            lines.append(f"🔴 <b>{name}</b>: світла нема вже {dur} (з {fmt_time(d['changed_at'])})")
    return "\n".join(lines)


def bot_poller(store, outbox):
    """Відповідає на /start (показує chat ID) і /status."""
    offset = 0
    while True:
        try:
            res = tg_call("getUpdates", {"offset": offset, "timeout": 50, "allowed_updates": ["message"]}, timeout=60)
            for upd in res.get("result", []):
                offset = upd["update_id"] + 1
                msg = upd.get("message") or {}
                text = (msg.get("text") or "").strip()
                chat_id = msg.get("chat", {}).get("id")
                if chat_id is None or not text.startswith("/"):
                    continue
                cmd = text.split()[0].split("@")[0].lower()
                if cmd in ("/start", "/id", "/help"):
                    outbox.send(
                        chat_id,
                        f"Ваш chat ID: <code>{chat_id}</code>\n\n"
                        "Введіть його в налаштуваннях ESP32 (мережа «Svitlo-Setup» або http://svitlo.local).\n"
                        "/status — поточний стан світла",
                    )
                elif cmd == "/status":
                    outbox.send(chat_id, status_text(chat_id, store))
        except Exception as e:
            log.warning("getUpdates: %s", e)
            time.sleep(5)


# ---------- HTTP ----------

def make_handler(monitor, outbox):
    class Handler(BaseHTTPRequestHandler):
        server_version = "svitlo/1.0"

        def log_message(self, fmt, *args):
            log.debug("%s %s", self.address_string(), fmt % args)

        def reply(self, code, obj):
            body = json.dumps(obj, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/health":
                self.reply(200, {"ok": True})
            else:
                self.reply(404, {"error": "not found"})

        def do_POST(self):
            if self.path not in ("/api/ping", "/api/test"):
                return self.reply(404, {"error": "not found"})
            try:
                length = int(self.headers.get("Content-Length", "0"))
                data = json.loads(self.rfile.read(min(length, 4096)) or b"{}")
            except (ValueError, json.JSONDecodeError):
                return self.reply(400, {"error": "bad json"})
            if data.get("key") != API_KEY:
                return self.reply(403, {"error": "bad key"})
            device_id = str(data.get("device_id", ""))[:64]
            chat_id = str(data.get("chat_id", ""))[:64]
            name = str(data.get("name", ""))[:40]
            if not device_id or not chat_id:
                return self.reply(400, {"error": "device_id and chat_id required"})

            if self.path == "/api/ping":
                monitor.ping(device_id, chat_id, name)
                log.debug("ping %s rssi=%s uptime=%s", device_id, data.get("rssi"), data.get("uptime"))
            else:
                try:
                    tg_call("sendMessage", {
                        "chat_id": chat_id,
                        "text": f"🔔 {title(name)}Тестове повідомлення. Все працює!",
                        "parse_mode": "HTML",
                    })
                except urllib.error.HTTPError as e:
                    desc = json.loads(e.read().decode() or "{}").get("description", str(e))
                    return self.reply(502, {"error": f"Telegram: {desc}"})
                except Exception as e:
                    return self.reply(502, {"error": f"Telegram недоступний: {e}"})
            self.reply(200, {"ok": True})

    return Handler


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if not BOT_TOKEN:
        raise SystemExit("Задайте BOT_TOKEN (у server/.env або змінній оточення)")
    store = Store(DB_PATH)
    outbox = Outbox()
    monitor = Monitor(store, outbox)
    threading.Thread(target=bot_poller, args=(store, outbox), daemon=True).start()
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), make_handler(monitor, outbox))
    log.info("Сервер слухає порт %d, таймаут %d с", PORT, OFFLINE_TIMEOUT)
    httpd.serve_forever()


if __name__ == "__main__":
    main()
