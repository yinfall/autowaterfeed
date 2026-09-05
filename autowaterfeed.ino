/*
 * autowaterfeed - 猫用自动喂水器（ESP8266）
 *
 * 功能：
 *   - 继电器控制水泵/电磁阀，上电默认断开，防止开机误出水
 *   - Web 控制页（根路径 /）：当前时间、喂水状态、计划列表、"立即喂水"按钮
 *   - Web 配置页（/config）：WiFi、时区、最多 4 个喂水时段（可独立启用）、每次喂水时长（秒）
 *   - NTP 校时（pool.ntp.org），按时段自动喂水，非阻塞调度
 *
 * 硬件：
 *   - 继电器模块（默认低电平触发，IN 低电平=吸合/开泵）
 *   - 修改 RELAY_PIN 匹配实际接线；若模块为高电平触发，对调 RELAY_ON/RELAY_OFF
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

#include <WiFiUdp.h>
#include <NTPClient.h>

#include <WebConfig.h>

// ---------------- 硬件配置 ----------------
// 继电器控制引脚（按实际接线修改）。
// 建议避开 GPIO0/GPIO2（影响启动模式），常用：GPIO4(D2)、GPIO5(D1)、GPIO12(D6)、GPIO14(D5)
#define RELAY_PIN 4
// 常见继电器模块为低电平触发：LOW=吸合(出水)，HIGH=断开(停止)
// 若你的模块是高电平触发，将下面两行对调
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// 喂水时段数量 / 时长上限
#define FEED_SLOTS 4
#define MAX_DURATION_S 600

// ---------------- 全局对象 ----------------
String formattedTime;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 28800);

ESP8266WebServer server(80);
WebConfig conf;

// ---------------- 喂水调度状态 ----------------
struct FeedSlot {
  bool enabled;
  int hh;
  int mm;
};

FeedSlot slots[FEED_SLOTS];
int feedDurationS = 10;          // 每次喂水时长（秒）
bool feeding = false;            // 是否正在喂水
unsigned long feedUntil = 0;     // 本次喂水结束时刻（millis）
int lastTriggerMinute = -1;      // 上次触发喂水的分钟（避免同一分钟重复触发）
bool feedNowRequested = false;   // 立即喂水请求（由 /feed 置位，主循环执行）

// ---------------- Web 配置项描述 ----------------
String params = "["
  "{"
  "'name':'ssid',"
  "'label':'WLAN SSID',"
  "'type':" + String(INPUTTEXT) + ","
  "'default':''"
  "},"
  "{"
  "'name':'pwd',"
  "'label':'WLAN Password',"
  "'type':" + String(INPUTPASSWORD) + ","
  "'default':''"
  "},"
  "{"
  "'name':'timezone',"
  "'label':'UTC 时区',"
  "'type':" + String(INPUTNUMBER) + ","
  "'min':-12,'max':12,"
  "'default':'8'"
  "},"
  "{"
  "'name':'feed1en',"
  "'label':'喂水时段1 - 启用',"
  "'type':" + String(INPUTCHECKBOX) + ","
  "'default':'1'"
  "},"
  "{"
  "'name':'feed1t',"
  "'label':'喂水时段1 - 时间',"
  "'type':" + String(INPUTTIME) + ","
  "'default':'08:00'"
  "},"
  "{"
  "'name':'feed2en',"
  "'label':'喂水时段2 - 启用',"
  "'type':" + String(INPUTCHECKBOX) + ","
  "'default':'1'"
  "},"
  "{"
  "'name':'feed2t',"
  "'label':'喂水时段2 - 时间',"
  "'type':" + String(INPUTTIME) + ","
  "'default':'18:00'"
  "},"
  "{"
  "'name':'feed3en',"
  "'label':'喂水时段3 - 启用',"
  "'type':" + String(INPUTCHECKBOX) + ","
  "'default':'0'"
  "},"
  "{"
  "'name':'feed3t',"
  "'label':'喂水时段3 - 时间',"
  "'type':" + String(INPUTTIME) + ","
  "'default':'12:00'"
  "},"
  "{"
  "'name':'feed4en',"
  "'label':'喂水时段4 - 启用',"
  "'type':" + String(INPUTCHECKBOX) + ","
  "'default':'0'"
  "},"
  "{"
  "'name':'feed4t',"
  "'label':'喂水时段4 - 时间',"
  "'type':" + String(INPUTTIME) + ","
  "'default':'21:00'"
  "},"
  "{"
  "'name':'feedduration',"
  "'label':'每次喂水时长(秒)',"
  "'type':" + String(INPUTNUMBER) + ","
  "'min':1,'max':600,"
  "'default':'10'"
  "}"
  "]";

// ---------------- 配置解析 ----------------
void parseFeedConfig() {
  feedDurationS = constrain(conf.getInt("feedduration"), 1, MAX_DURATION_S);

  for (int i = 0; i < FEED_SLOTS; i++) {
    char name[16];
    sprintf(name, "feed%den", i + 1);
    slots[i].enabled = conf.getBool(name);
    sprintf(name, "feed%dt", i + 1);
    String t = conf.getString(name);
    int colon = t.indexOf(':');
    if (colon > 0) {
      slots[i].hh = t.substring(0, colon).toInt();
      slots[i].mm = t.substring(colon + 1).toInt();
    } else {
      slots[i].hh = -1;
      slots[i].mm = -1;
    }
  }

  timeClient.setTimeOffset((int)conf.getInt("timezone") * 3600);
}

// 配置页保存后回调
void onConfigSaved(String results) {
  parseFeedConfig();
  Serial.println("*********** 配置已保存 ************");
  for (int i = 0; i < FEED_SLOTS; i++) {
    if (slots[i].enabled)
      Serial.printf("  时段%d: %02d:%02d\n", i + 1, slots[i].hh, slots[i].mm);
  }
  Serial.printf("  每次时长: %d 秒\n", feedDurationS);
}

// ---------------- 喂水控制 ----------------
void relayOn()  { digitalWrite(RELAY_PIN, RELAY_ON); }
void relayOff() { digitalWrite(RELAY_PIN, RELAY_OFF); }

void startFeed() {
  if (feeding) return;  // 已在喂水则忽略新请求
  feeding = true;
  feedUntil = millis() + (unsigned long)feedDurationS * 1000UL;
  relayOn();
  Serial.printf("[feed] START %ds @ %s\n", feedDurationS, formattedTime.c_str());
}

void stopFeed() {
  if (!feeding) return;
  feeding = false;
  relayOff();
  Serial.printf("[feed] STOP @ %s\n", formattedTime.c_str());
}

// ---------------- Web 页面 ----------------
void handleRoot() {
  String scheduleHtml = "";
  for (int i = 0; i < FEED_SLOTS; i++) {
    if (slots[i].enabled) {
      char line[32];
      sprintf(line, "%02d:%02d", slots[i].hh, slots[i].mm);
      scheduleHtml += String(line) + "&nbsp;&nbsp;";
    }
  }
  if (scheduleHtml.length() == 0) scheduleHtml = "（未启用任何时段）";

  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=320'>"
    "<style>"
    "body{font-family:Arial,sans-serif;width:320px;margin:16px auto;color:#333;}"
    "h2{text-align:center;}"
    ".card{background:#f5f5f5;border-radius:12px;padding:14px;margin:12px 0;}"
    ".big{width:100%;font-size:20pt;padding:24px;border:none;border-radius:12px;"
    "background:#4CAF50;color:#fff;cursor:pointer;}"
    ".big:active{background:#388E3C;}"
    ".feed{color:#4CAF50;font-weight:bold;}"
    ".off{color:#999;}"
    ".cfg{display:block;text-align:center;margin-top:16px;color:#1976D2;}"
    "</style></head><body>"
    "<h2>自动喂水器</h2>"
    "<div class='card'>当前时间: <b>" + formattedTime + "</b><br>"
    "状态: " + String(feeding ? "<span class='feed'>喂水中…</span>" : "<span class='off'>待机</span>") + "</div>"
    "<form method='post' action='/feed'>"
    "<button class='big' type='submit'>立即喂水</button>"
    "</form>"
    "<div class='card'>喂水计划（每次 " + String(feedDurationS) + " 秒）:<br>"
    + scheduleHtml + "</div>"
    "<a class='cfg' href='/config'>打开配置页面</a>"
    "</body></html>";
  server.send(200, "text/html", h);
}

void handleFeedNow() {
  if (server.method() == HTTP_POST) {
    feedNowRequested = true;
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta http-equiv='refresh' content='1;url=/'>"
      "<style>body{font-family:Arial;text-align:center;padding-top:60px;}</style></head>"
      "<body><h2>已开始喂水</h2><p>1 秒后自动返回…</p></body></html>";
    server.send(200, "text/html", h);
  } else {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  }
}

void handleConfig() {
  conf.handleFormRequest(&server);
}

// ---------------- 主程序 ----------------
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();  // 上电默认断开，防止开机误出水

  Serial.begin(115200);
  Serial.println();
  Serial.println("=== autowaterfeed boot ===");

  conf.setDescription(params);
  conf.readConfig();
  conf.registerOnSave(onConfigSaved);
  parseFeedConfig();

  // 没有保存 WiFi 配置时，先开启开放热点供用户配置。
  WiFi.hostname(conf.getApName());
  String wifiSsid = conf.getString("ssid");
  String wifiPwd = conf.getString("pwd");
  if (wifiSsid.length() == 0) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(conf.getApName());
    Serial.println("WiFi is not configured");
    Serial.print("Config AP: ");
    Serial.println(conf.getApName());
    Serial.print("Config IP = ");
    Serial.println(WiFi.softAPIP());
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid, wifiPwd);
    Serial.print("Connecting WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    Serial.print("Connected, IP = ");
    Serial.println(WiFi.localIP());
  }

  // NTP 时间
  timeClient.begin();
  timeClient.setTimeOffset((int)conf.getInt("timezone") * 3600);
  timeClient.update();

  // mDNS: <apName>.local
  char dns[30];
  sprintf(dns, "%s.local", conf.getApName());
  if (MDNS.begin(dns)) {
    Serial.println("MDNS: " + String(dns));
  }

  // Web 服务
  server.on("/", HTTP_GET, handleRoot);
  server.on("/feed", handleFeedNow);
  server.on("/config", HTTP_ANY, handleConfig);
  server.begin();
  Serial.println("HTTP server on :80");
}

void loop() {
  timeClient.update();  // 保持时间同步（按需刷新，不会频繁发包）
  formattedTime = timeClient.getFormattedTime();

  int nowMinute = timeClient.getHours() * 60 + timeClient.getMinutes();

  if (feeding) {
    // 非阻塞计时：到点停止
    if ((long)(millis() - feedUntil) >= 0) {
      stopFeed();
    }
  } else {
    if (feedNowRequested) {
      feedNowRequested = false;
      startFeed();
    } else if (nowMinute != lastTriggerMinute) {
      // 检查各时段是否到达（同一分钟只触发一次）
      for (int i = 0; i < FEED_SLOTS; i++) {
        if (slots[i].enabled &&
            slots[i].hh == timeClient.getHours() &&
            slots[i].mm == timeClient.getMinutes()) {
          startFeed();
          lastTriggerMinute = nowMinute;
          break;
        }
      }
    }
  }

  server.handleClient();
  MDNS.update();
}
