// ESP32 «Світло-бот»: шле пінги на сервер, а сервер повідомляє в Telegram,
// коли світло зникає (пінги припинились) і коли з'являється (пінги відновились).

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "config.h"

struct Settings {
  String ssid;
  String pass;
  String chatId;
  String name;
};

Settings cfg;
Preferences prefs;
WebServer web(80);
DNSServer dns;
String deviceId;

bool apActive = false;
bool mdnsStarted = false;
unsigned long apStartedAt = 0;
unsigned long lastWifiAttemptAt = 0;
unsigned long wifiLostAt = 0;

bool pingedOnce = false;
unsigned long lastPingAt = 0;
unsigned long lastPingOkAt = 0;
int lastPingCode = 0;

bool rebootPending = false;
unsigned long rebootAt = 0;

String scanOptions;
unsigned long scanAt = 0;

// ---------- налаштування ----------

void loadSettings() {
  prefs.begin("svitlo", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.pass = prefs.getString("pass", "");
  cfg.chatId = prefs.getString("chat", DEFAULT_CHAT_ID);
  cfg.name = prefs.getString("name", "");
  prefs.end();
}

void saveSettings() {
  prefs.begin("svitlo", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("chat", cfg.chatId);
  prefs.putString("name", cfg.name);
  prefs.end();
}

void clearSettings() {
  prefs.begin("svitlo", false);
  prefs.clear();
  prefs.end();
}

bool isConfigured() { return cfg.ssid.length() > 0 && cfg.chatId.length() > 0; }

// ---------- утиліти ----------

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

String formatAgo(unsigned long ms) {
  unsigned long s = ms / 1000;
  if (s < 60) return String(s) + " с тому";
  if (s < 3600) return String(s / 60) + " хв тому";
  return String(s / 3600) + " год " + String((s % 3600) / 60) + " хв тому";
}

void setLed(bool on) {
#if LED_PIN >= 0
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#endif
}

// ---------- HTTP до сервера ----------

int postJson(const String& path, const String& body, String* response = nullptr) {
  String url = String(SERVER_URL) + path;
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  bool ok;
  if (url.startsWith("https://")) {
    secure.setInsecure();  // без перевірки сертифіката — простіше для домашнього проєкту
    ok = http.begin(secure, url);
  } else {
    ok = http.begin(plain, url);
  }
  if (!ok) return -1;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (response && code > 0) *response = http.getString();
  http.end();
  return code;
}

String baseJson() {
  return String("{\"key\":\"") + jsonEscape(API_KEY) + "\",\"device_id\":\"" + deviceId +
         "\",\"chat_id\":\"" + jsonEscape(cfg.chatId) + "\",\"name\":\"" + jsonEscape(cfg.name) +
         "\",\"uptime\":" + String(millis() / 1000) + ",\"rssi\":" + String(WiFi.RSSI()) + "}";
}

void sendPing() {
  lastPingAt = millis();
  pingedOnce = true;
  lastPingCode = postJson("/api/ping", baseJson());
  if (lastPingCode == 200) lastPingOkAt = millis();
  Serial.printf("Пінг: %d\n", lastPingCode);
}

// ---------- Wi-Fi ----------

void startAP() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : nullptr);
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
  apStartedAt = millis();
  Serial.printf("Точка доступу \"%s\" піднята, http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
}

void stopAP() {
  if (!apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
  Serial.println("Точку доступу вимкнено");
}

bool connectWifi(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  Serial.printf("Підключення до \"%s\"", cfg.ssid.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void handleWifi() {
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    wifiLostAt = 0;
    if (!mdnsStarted && MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.printf("Підключено, IP %s, http://%s.local/\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    }
    // Роутер з'явився, а в портал ніхто не зайшов — гасимо точку доступу.
    if (apActive && WiFi.softAPgetStationNum() == 0) stopAP();
    if (!pingedOnce || millis() - lastPingAt >= PING_INTERVAL_MS) sendPing();
    return;
  }

  if (!isConfigured()) return;
  if (wifiLostAt == 0) wifiLostAt = millis();

  if (!apActive && millis() - wifiLostAt > WIFI_LOST_AP_AFTER_MS) startAP();

  // Періодично пробуємо знову. Поки хтось сидить у порталі — не чіпаємо,
  // бо спроба підключення перемикає канал і рве з'єднання з точкою доступу.
  if (millis() - lastWifiAttemptAt > 30000 && (!apActive || WiFi.softAPgetStationNum() == 0)) {
    lastWifiAttemptAt = millis();
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  }
}

// ---------- веб-інтерфейс ----------

const char PAGE_HEAD[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="uk"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Світло-бот</title>
<style>
body{font-family:system-ui,sans-serif;background:#f4f5f7;color:#222;margin:0;padding:16px}
.card{max-width:440px;margin:0 auto 16px;background:#fff;border-radius:12px;padding:18px;box-shadow:0 1px 4px #0002}
h1{font-size:22px;margin:0 0 12px}h2{font-size:17px;margin:0 0 10px}
label{display:block;margin:12px 0 4px;font-weight:600}
input{width:100%;box-sizing:border-box;padding:10px;font-size:16px;border:1px solid #ccc;border-radius:8px}
button{width:100%;margin-top:16px;padding:12px;font-size:16px;border:0;border-radius:8px;background:#f5a623;color:#fff;font-weight:600}
button.sec{background:#4a6fa5}button.del{background:#c0392b}
small{color:#666}.row{display:flex;justify-content:space-between;padding:3px 0;border-bottom:1px solid #eee}
.ok{color:#1e8e3e}.bad{color:#c0392b}
</style></head><body>)HTML";

void refreshScan() {
  if (scanOptions.length() && millis() - scanAt < 20000) return;
  int n = WiFi.scanNetworks();
  scanOptions = "";
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || scanOptions.indexOf(">" + htmlEscape(ssid) + "<") >= 0) continue;
    scanOptions += "<option value=\"" + htmlEscape(ssid) + "\">" + htmlEscape(ssid) + "</option>";
  }
  WiFi.scanDelete();
  scanAt = millis();
}

void sendPage(const String& body) {
  String html = FPSTR(PAGE_HEAD);
  html += body;
  html += "</body></html>";
  web.send(200, "text/html; charset=utf-8", html);
}

String statusRow(const String& k, const String& v) {
  return "<div class=row><span>" + k + "</span><b>" + v + "</b></div>";
}

void handleRoot() {
  refreshScan();
  bool connected = WiFi.status() == WL_CONNECTED;
  String b = "<div class=card><h1>💡 Світло-бот</h1>";
  b += statusRow("ID пристрою", deviceId);
  String wifi = "<span class=bad>не підключено</span>";
  if (connected) wifi = "<span class=ok>" + htmlEscape(WiFi.SSID()) + " (" + String(WiFi.RSSI()) + " dBm)</span>";
  b += statusRow("Wi-Fi", wifi);
  if (connected) b += statusRow("IP", WiFi.localIP().toString());
  String ping = "ще не було";
  if (pingedOnce) {
    if (lastPingCode == 200) {
      ping = "<span class=ok>OK</span>";
    } else {
      ping = "<span class=bad>помилка " + String(lastPingCode) + "</span>";
    }
    ping += ", " + formatAgo(millis() - lastPingAt);
  }
  b += statusRow("Пінг на сервер", ping);
  b += "</div>";

  b += "<div class=card><h2>Налаштування</h2><form method=post action=/save>";
  b += "<label>Wi-Fi мережа</label><input name=ssid list=nets required value=\"" + htmlEscape(cfg.ssid) +
       "\" placeholder=\"Виберіть або введіть\"><datalist id=nets>" + scanOptions + "</datalist>";
  b += "<label>Пароль Wi-Fi</label><input name=pass type=password placeholder=\"" +
       String(cfg.pass.length() ? "залиште порожнім, щоб не змінювати" : "") + "\">";
  b += "<label>Telegram chat ID</label><input name=chat required value=\"" + htmlEscape(cfg.chatId) +
       "\" placeholder=\"123456789 або -100…\">";
  b += "<small>Напишіть боту <b>/start</b> — він відповість вашим chat ID. "
       "Для групи: додайте бота в групу і напишіть там /start.</small>";
  b += "<label>Назва (необов'язково)</label><input name=name maxlength=40 value=\"" + htmlEscape(cfg.name) +
       "\" placeholder=\"Дім, Дача…\">";
  b += "<button type=submit>Зберегти і перезавантажити</button></form></div>";

  if (connected && cfg.chatId.length()) {
    b += "<div class=card><form method=post action=/test><button class='sec'>Надіслати тестове повідомлення</button>"
         "</form></div>";
  }
  b += "<div class=card><form method=post action=/reset onsubmit=\"return confirm('Стерти всі налаштування?')\">"
       "<button class=del>Скинути налаштування</button></form></div>";
  sendPage(b);
}

bool validChatId(const String& s) {
  if (s.length() == 0) return false;
  if (s[0] == '@') return s.length() > 1;  // публічний канал
  for (size_t i = 0; i < s.length(); i++) {
    if (!(isDigit(s[i]) || (i == 0 && s[i] == '-'))) return false;
  }
  return true;
}

void handleSave() {
  String ssid = web.arg("ssid");
  String pass = web.arg("pass");
  String chat = web.arg("chat");
  String name = web.arg("name");
  ssid.trim();
  chat.trim();
  name.trim();

  if (ssid.length() == 0 || !validChatId(chat)) {
    sendPage("<div class=card><h2 class=bad>Помилка</h2><p>Перевірте назву мережі та chat ID "
             "(лише цифри, може починатися з «-», або @канал).</p><a href=/>Назад</a></div>");
    return;
  }
  if (pass.length() == 0 && ssid == cfg.ssid) pass = cfg.pass;

  cfg.ssid = ssid;
  cfg.pass = pass;
  cfg.chatId = chat;
  cfg.name = name;
  saveSettings();

  sendPage("<div class=card><h2 class=ok>Збережено ✔</h2><p>Пристрій перезавантажується і підключиться до <b>" +
           htmlEscape(ssid) + "</b>.</p><p>Коли все запрацює, бот надішле повідомлення в Telegram. "
           "Якщо не вдасться підключитись — знову з'явиться мережа <b>" AP_SSID "</b>.</p>"
           "<p><small>Потім налаштування доступні за адресою http://" HOSTNAME ".local</small></p></div>");
  rebootPending = true;
  rebootAt = millis() + 2000;
}

void handleTest() {
  String resp;
  int code = postJson("/api/test", baseJson(), &resp);
  String msg = "<h2 class=ok>Надіслано ✔</h2><p>Перевірте Telegram.</p>";
  if (code != 200) msg = "<h2 class=bad>Не вдалося</h2><p>Код: " + String(code) + "<br>" + htmlEscape(resp) + "</p>";
  sendPage("<div class=card>" + msg + "<a href=/>Назад</a></div>");
}

void handleReset() {
  clearSettings();
  sendPage("<div class=card><h2>Налаштування стерто</h2><p>Перезавантаження… Підключіться до мережі <b>" AP_SSID
           "</b>.</p></div>");
  rebootPending = true;
  rebootAt = millis() + 2000;
}

void handleNotFound() {
  // Captive portal: телефон перевіряє інтернет — відправляємо на головну сторінку.
  String target = "/";
  if (apActive && web.client().localIP() == WiFi.softAPIP()) target = "http://" + WiFi.softAPIP().toString() + "/";
  web.sendHeader("Location", target, true);
  web.send(302, "text/plain", "");
}

// ---------- кнопка скидання ----------

void handleResetButton() {
  static unsigned long pressedAt = 0;
  if (digitalRead(RESET_PIN) == LOW) {
    if (pressedAt == 0) pressedAt = millis();
    if (millis() - pressedAt > RESET_HOLD_MS) {
      Serial.println("Скидання налаштувань кнопкою");
      clearSettings();
      for (int i = 0; i < 10; i++) {
        setLed(i % 2);
        delay(100);
      }
      ESP.restart();
    }
  } else {
    pressedAt = 0;
  }
}

void updateLed() {
  // Точка доступу — блимає; пінг успішний — світить; інакше — не світить.
  if (apActive) {
    setLed((millis() / 500) % 2);
  } else {
    setLed(pingedOnce && lastPingCode == 200);
  }
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(RESET_PIN, INPUT_PULLUP);
#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
#endif

  char id[20];
  snprintf(id, sizeof(id), "esp32-%012llx", (unsigned long long)ESP.getEfuseMac());
  deviceId = id;
  Serial.printf("\nСвітло-бот, ID %s\n", deviceId.c_str());

  loadSettings();
  WiFi.setHostname(HOSTNAME);

  if (!isConfigured()) {
    startAP();
  } else if (!connectWifi(WIFI_CONNECT_TIMEOUT_MS)) {
    // Після відключення світла роутер може вантажитись довше за ESP —
    // піднімаємо портал, але продовжуємо пробувати підключитись.
    startAP();
    wifiLostAt = millis();
    lastWifiAttemptAt = millis();
  }

  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/test", HTTP_POST, handleTest);
  web.on("/reset", HTTP_POST, handleReset);
  web.onNotFound(handleNotFound);
  web.begin();
}

void loop() {
  if (apActive) dns.processNextRequest();
  web.handleClient();
  handleResetButton();
  updateLed();

  if (rebootPending && (long)(millis() - rebootAt) >= 0) ESP.restart();
  if (!rebootPending) handleWifi();
  delay(2);
}
