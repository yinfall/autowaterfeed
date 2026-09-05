/*
 * autowaterfeed - 猫用/宠物智能自动喂水器（ESP8266 / ESP-01S 专属优化版）
 *
 * 核心功能与优化亮点：
 *   1. 硬件与继电器安全：
 *      - 针对 ESP-01S 硬件引脚定制（GPIO 0 控制继电器，GPIO 2 驱动板载指示灯）
 *      - 上电首行初始化锁定关闭电平，软硬件双重抑制上电误出水
 *      - 增加硬件防溢水强制硬超时看门狗（HARD_MAX_FEED_SECONDS），绝无长流水隐患
 *      - 支持随时“立即停止出水”紧急按键
 *
 *   2. 网络鲁棒性与离线自愈：
 *      - 开启 WiFi.setAutoReconnect 和 WIFI_NONE_SLEEP
 *      - AP+STA 双模容灾：开机若找不到路由器，开启热点供配网的同时，在后台保持自动重连
 *      - 彻底解决断电重启后因路由器启动慢而永久卡在 AP 模式的常见痛点
 *
 *   3. 极速时钟与防误触发：
 *      - 默认采用国内高速低延迟源（阿里云 NTP：ntp.aliyun.com），支持多源自动轮换重试
 *      - 严格的时间有效性校验，未完成 NTP 同步前坚决不触发喂水，杜绝 00:00:00 误触发
 *      - 精确记录并展示“上次出水记录”和“下次预计出水时间”
 *
 *   4. 现代化轻量响应式交互：
 *      - HTML/CSS/JS 全量静态存入 Flash（PROGMEM），运行时 0 堆内存占用，远离 OOM 崩溃
 *      - 采用轻量 RESTful API（/api/status、/api/feed、/api/stop）
 *      - 前端页面无刷新平滑交互，出水时显示实时秒数倒计时与动态水波效果
 *      - 修复配置页面保存后连接挂起的 Bug，提供优雅的保存成功跳转提示
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

#include <WiFiUdp.h>
#include <NTPClient.h>

#include <WebConfig.h>

// ---------------- 硬件配置 ----------------
// ESP-01S 继电器模块走线固定为 GPIO 0 (IO0)
#define RELAY_PIN 0

// 市面主流 ESP-01S 继电器底座为低电平触发：
// 若你的模块是高电平触发，将下面两行对调
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ESP-01S 板载蓝色 LED (GPIO 2, 低电平点亮)
#define USE_LED_INDICATOR 1
#define LED_PIN 2

// 软件硬安全限制：单次最长出水时间（秒），防止任何异常导致长流水
#define HARD_MAX_FEED_SECONDS 180

// 喂水时段数量 / 时长上限
#define FEED_SLOTS 4
#define MAX_DURATION_S 600

// ---------------- 全局对象 ----------------
WiFiUDP ntpUDP;
// 默认使用阿里云 NTP，极速低延迟；备用服务器见 maintainNTP()
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 28800, 60000);

ESP8266WebServer server(80);
WebConfig conf;

// ---------------- 喂水调度与状态 ----------------
struct FeedSlot {
  bool enabled;
  int hh;
  int mm;
};

FeedSlot slots[FEED_SLOTS];
int feedDurationS = 10;            // 每次喂水时长（秒）
bool feeding = false;              // 是否正在出水
unsigned long feedStartTime = 0;   // 本次喂水开始时刻（millis）
unsigned long feedUntil = 0;       // 本次喂水结束时刻（millis）
int lastTriggerMinute = -1;        // 当天上次触发喂水的分钟数（0~1439）
String lastFeedInfo = "暂无记录";   // 上次喂水记录信息

// ---------------- Web 配置项描述 ----------------
// 保持原有字段名与配置完全兼容
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
  "'min':-12,'max':14,"
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
  "'min':1,'max':" + String(MAX_DURATION_S) + ","
  "'default':'10'"
  "}"
  "]";

// ---------------- 前端页面 (嵌入 Flash / PROGMEM，0 内存占用) ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>智能宠物喂水器</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif;
  background: #f1f5f9;
  color: #1e293b;
  min-height: 100vh;
  padding: 16px;
  display: flex;
  justify-content: center;
}
.app { width: 100%; max-width: 400px; }
.header { text-align: center; margin-bottom: 14px; padding-top: 4px; }
.header h1 { font-size: 21px; font-weight: 700; color: #0f172a; display: flex; align-items: center; justify-content: center; gap: 8px; }
.header p { font-size: 13px; color: #64748b; margin-top: 3px; }
.card {
  background: #ffffff;
  border-radius: 16px;
  padding: 16px 14px;
  margin-bottom: 12px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.05);
}
.status-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px; }
.badge {
  display: inline-flex;
  align-items: center;
  padding: 4px 10px;
  border-radius: 20px;
  font-size: 12px;
  font-weight: 600;
}
.badge-idle { background: #dcfce7; color: #15803d; }
.badge-feeding { background: #dbeafe; color: #1d4ed8; animation: pulse 1.5s infinite; }
@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.6; } }
.clock { font-size: 13px; color: #64748b; font-variant-numeric: tabular-nums; }
.main-display { text-align: center; padding: 6px 0 14px; }
.main-state { font-size: 13px; color: #64748b; margin-bottom: 2px; }
.countdown { font-size: 32px; font-weight: 800; color: #2563eb; letter-spacing: -0.5px; }
.btn {
  width: 100%;
  padding: 15px;
  border: none;
  border-radius: 12px;
  font-size: 17px;
  font-weight: 700;
  color: #ffffff;
  cursor: pointer;
  transition: all 0.2s ease;
  user-select: none;
}
.btn:active { transform: scale(0.98); }
.btn-feed { background: linear-gradient(135deg, #10b981, #059669); box-shadow: 0 4px 14px rgba(16, 185, 129, 0.3); }
.btn-stop { background: linear-gradient(135deg, #ef4444, #dc2626); box-shadow: 0 4px 14px rgba(239, 68, 68, 0.3); }
.info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.info-item { background: #f8fafc; padding: 8px 10px; border-radius: 10px; }
.info-label { font-size: 11px; color: #64748b; margin-bottom: 2px; }
.info-val { font-size: 13px; font-weight: 600; color: #0f172a; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.section-title { font-size: 13px; font-weight: 700; color: #334155; margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center; }
.slots-list { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.slot-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 10px; border-radius: 10px; font-size: 13px; }
.slot-on { background: #f0fdf4; border: 1px solid #bbf7d0; color: #166534; }
.slot-off { background: #f8fafc; border: 1px dashed #cbd5e1; color: #94a3b8; }
.slot-time { font-weight: 700; font-size: 14px; }
.cfg-btn {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  width: 100%;
  padding: 12px;
  border-radius: 12px;
  background: #ffffff;
  color: #2563eb;
  font-size: 14px;
  font-weight: 600;
  text-decoration: none;
  box-shadow: 0 2px 10px rgba(0,0,0,0.03);
  transition: background 0.2s;
}
.cfg-btn:hover { background: #f8fafc; }
.footer { text-align: center; font-size: 11px; color: #94a3b8; margin-top: 12px; }
</style>
</head>
<body>
<div class="app">
  <div class="header">
    <h1>🐱 智能宠物喂水器</h1>
    <p>ESP8266 Auto Feeder</p>
  </div>

  <div class="card">
    <div class="status-header">
      <span id="statusBadge" class="badge badge-idle">● 待机就绪</span>
      <span id="curTime" class="clock">--:--:--</span>
    </div>
    <div class="main-display">
      <div id="mainState" class="main-state">当前状态</div>
      <div id="countdownDisplay" class="countdown">待机中</div>
    </div>
    <button id="actionBtn" class="btn btn-feed" onclick="handleAction()">💧 立即喂水</button>
  </div>

  <div class="card">
    <div class="section-title">📊 运行状态</div>
    <div class="info-grid">
      <div class="info-item">
        <div class="info-label">下次计划</div>
        <div id="nextFeed" class="info-val">计算中...</div>
      </div>
      <div class="info-item">
        <div class="info-label">上次出水</div>
        <div id="lastFeed" class="info-val">暂无记录</div>
      </div>
      <div class="info-item">
        <div class="info-label">单次时长</div>
        <div id="feedDur" class="info-val">10 秒</div>
      </div>
      <div class="info-item">
        <div class="info-label">系统运行</div>
        <div id="uptime" class="info-val">--</div>
      </div>
      <div class="info-item">
        <div class="info-label">网络 / 信号</div>
        <div id="wifi" class="info-val">--</div>
      </div>
      <div class="info-item">
        <div class="info-label">设备 IP</div>
        <div id="ip" class="info-val">--</div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="section-title">⏰ 定时喂水计划</div>
    <div id="slotsContainer" class="slots-list"></div>
  </div>

  <a href="/config" class="cfg-btn">⚙️ 修改 WiFi 与计划设置</a>
  <div class="footer">AutoWaterFeed v2.0 · ESP-01S</div>
</div>

<script>
let isFeeding = false;
let polling = false;

async function fetchStatus() {
  if (polling) return;
  polling = true;
  try {
    const res = await fetch('/api/status');
    if (!res.ok) throw new Error();
    const d = await res.json();
    updateUI(d);
  } catch(e) {
  } finally {
    polling = false;
  }
}

function updateUI(d) {
  isFeeding = d.feeding;
  document.getElementById('curTime').innerText = d.time;
  document.getElementById('nextFeed').innerText = d.nextFeed;
  document.getElementById('lastFeed').innerText = d.lastFeed;
  document.getElementById('feedDur').innerText = d.duration + ' 秒';
  document.getElementById('uptime').innerText = d.uptime;
  document.getElementById('ip').innerText = d.ip;
  document.getElementById('wifi').innerText = (d.wifi ? d.wifi + ' (' + d.rssi + 'dBm)' : 'AP 模式');

  const badge = document.getElementById('statusBadge');
  const actionBtn = document.getElementById('actionBtn');
  const mainState = document.getElementById('mainState');
  const countdownDisplay = document.getElementById('countdownDisplay');

  if (d.feeding) {
    badge.className = 'badge badge-feeding';
    badge.innerText = '● 正在出水...';
    mainState.innerText = '出水倒计时';
    countdownDisplay.innerText = d.remaining + ' 秒';
    actionBtn.className = 'btn btn-stop';
    actionBtn.innerText = '⏹️ 立即停止出水';
  } else {
    badge.className = 'badge badge-idle';
    badge.innerText = '● 待机就绪';
    mainState.innerText = '设备状态';
    countdownDisplay.innerText = '待机中';
    actionBtn.className = 'btn btn-feed';
    actionBtn.innerText = '💧 立即喂水 (' + d.duration + 's)';
  }

  if (d.slots && d.slots.length) {
    let html = '';
    d.slots.forEach((s, idx) => {
      html += `<div class="slot-item ${s.en ? 'slot-on' : 'slot-off'}">
        <div>
          <div style="font-size:11px;opacity:0.75">时段 ${idx + 1}</div>
          <div class="slot-time">${s.t}</div>
        </div>
        <div style="font-weight:600">${s.en ? '已开启' : '已禁用'}</div>
      </div>`;
    });
    document.getElementById('slotsContainer').innerHTML = html;
  }
}

async function handleAction() {
  const btn = document.getElementById('actionBtn');
  btn.disabled = true;
  try {
    if (isFeeding) {
      await fetch('/api/stop', { method: 'POST' });
    } else {
      await fetch('/api/feed', { method: 'POST' });
    }
  } catch(e) {}
  btn.disabled = false;
  fetchStatus();
}

setInterval(fetchStatus, 1500);
window.onload = fetchStatus;
</script>
</body>
</html>
)rawliteral";

// ---------------- 实用工具函数 ----------------
bool isTimeValid() {
  return timeClient.isTimeSet() && (timeClient.getEpochTime() > 100000000UL);
}

String getUptimeStr() {
  unsigned long s = millis() / 1000UL;
  int d = s / 86400;
  s %= 86400;
  int h = s / 3600;
  s %= 3600;
  int m = s / 60;
  int sec = s % 60;
  char buf[32];
  if (d > 0) {
    snprintf(buf, sizeof(buf), "%d天%d时%d分", d, h, m);
  } else if (h > 0) {
    snprintf(buf, sizeof(buf), "%d时%d分%d秒", h, m, sec);
  } else {
    snprintf(buf, sizeof(buf), "%d分%d秒", m, sec);
  }
  return String(buf);
}

String getNextFeedStr() {
  if (!isTimeValid()) return "等待网络校时...";
  int curMin = timeClient.getHours() * 60 + timeClient.getMinutes();

  int minToday = 9999;
  int minTomorrow = 9999;
  bool anyEnabled = false;

  for (int i = 0; i < FEED_SLOTS; i++) {
    if (slots[i].enabled && slots[i].hh >= 0 && slots[i].mm >= 0) {
      anyEnabled = true;
      int slotMin = slots[i].hh * 60 + slots[i].mm;
      if (slotMin > curMin && slotMin < minToday) {
        minToday = slotMin;
      }
      if (slotMin < minTomorrow) {
        minTomorrow = slotMin;
      }
    }
  }

  if (!anyEnabled) return "未启用任何时段";

  char buf[32];
  if (minToday != 9999) {
    snprintf(buf, sizeof(buf), "今日 %02d:%02d", minToday / 60, minToday % 60);
  } else if (minTomorrow != 9999) {
    snprintf(buf, sizeof(buf), "次日 %02d:%02d", minTomorrow / 60, minTomorrow % 60);
  } else {
    return "无即将执行的计划";
  }
  return String(buf);
}

// ---------------- 配置解析 ----------------
void parseFeedConfig() {
  feedDurationS = constrain(conf.getInt("feedduration"), 1, MAX_DURATION_S);

  for (int i = 0; i < FEED_SLOTS; i++) {
    char name[16];
    snprintf(name, sizeof(name), "feed%den", i + 1);
    slots[i].enabled = conf.getBool(name);
    snprintf(name, sizeof(name), "feed%dt", i + 1);
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

  int tz = conf.getInt("timezone");
  if (tz < -12 || tz > 14) tz = 8;
  timeClient.setTimeOffset(tz * 3600);
}

// 配置页保存后回调：返回用户友好的确认页，解决网页卡死问题
void onConfigSaved(String results) {
  parseFeedConfig();
  Serial.println(F("*********** 配置已保存 ************"));
  for (int i = 0; i < FEED_SLOTS; i++) {
    if (slots[i].enabled)
      Serial.printf("  时段%d: %02d:%02d\n", i + 1, slots[i].hh, slots[i].mm);
  }
  Serial.printf("  每次时长: %d 秒\n", feedDurationS);

  String h = F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
               "<meta http-equiv='refresh' content='2;url=/'>"
               "<style>"
               "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:#f0f4f8;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}"
               ".box{background:#fff;padding:28px 24px;border-radius:16px;box-shadow:0 8px 24px rgba(0,0,0,0.08);text-align:center;max-width:320px;width:90%;}"
               "h2{color:#10b981;margin-top:0;font-size:22px;}"
               "p{color:#64748b;font-size:14px;margin:12px 0 20px;}"
               "a{display:inline-block;padding:10px 20px;background:#3b82f6;color:#fff;border-radius:8px;text-decoration:none;font-weight:600;font-size:14px;}"
               "</style></head><body>"
               "<div class='box'><h2>✓ 配置已成功保存</h2><p>参数已生效，2 秒后自动返回首页...</p><a href='/'>立即返回首页</a></div>"
               "</body></html>");
  server.send(200, "text/html", h);
}

// ---------------- 继电器硬件动作 ----------------
void relayOn() {
  digitalWrite(RELAY_PIN, RELAY_ON);
#if USE_LED_INDICATOR
  digitalWrite(LED_PIN, LOW); // 喂水时点亮板载指示灯
#endif
}

void relayOff() {
  digitalWrite(RELAY_PIN, RELAY_OFF);
#if USE_LED_INDICATOR
  digitalWrite(LED_PIN, HIGH); // 待机时熄灭板载指示灯
#endif
}

void startFeed(const char* source) {
  if (feeding) return;
  feeding = true;
  feedStartTime = millis();
  feedUntil = feedStartTime + (unsigned long)feedDurationS * 1000UL;
  relayOn();

  String tStr = isTimeValid() ? timeClient.getFormattedTime() : "未校准";
  lastFeedInfo = tStr + " (" + String(source) + ", " + String(feedDurationS) + "s)";
  Serial.printf("[feed] START %ds [%s] @ %s\n", feedDurationS, source, tStr.c_str());
}

void stopFeed() {
  if (!feeding) return;
  feeding = false;
  relayOff();
  Serial.printf("[feed] STOP @ %s\n", isTimeValid() ? timeClient.getFormattedTime().c_str() : "未校准");
}

// ---------------- Web 服务接口 ----------------
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleApiStatus() {
  int remaining = 0;
  if (feeding) {
    long remMs = (long)(feedUntil - millis());
    remaining = (remMs > 0) ? (int)((remMs + 999) / 1000) : 0;
  }

  String json = "{";
  json += "\"feeding\":" + String(feeding ? "true" : "false") + ",";
  json += "\"remaining\":" + String(remaining) + ",";
  json += "\"duration\":" + String(feedDurationS) + ",";
  json += "\"time\":\"" + (isTimeValid() ? timeClient.getFormattedTime() : "时间校准中...") + "\",";
  json += "\"timeValid\":" + String(isTimeValid() ? "true" : "false") + ",";
  json += "\"lastFeed\":\"" + lastFeedInfo + "\",";
  json += "\"nextFeed\":\"" + getNextFeedStr() + "\",";
  json += "\"wifi\":\"" + WiFi.SSID() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
  json += "\"uptime\":\"" + getUptimeStr() + "\",";
  json += "\"slots\":[";
  for (int i = 0; i < FEED_SLOTS; i++) {
    char tBuf[16];
    if (slots[i].hh >= 0 && slots[i].mm >= 0) {
      snprintf(tBuf, sizeof(tBuf), "%02d:%02d", slots[i].hh, slots[i].mm);
    } else {
      snprintf(tBuf, sizeof(tBuf), "--:--");
    }
    json += "{\"en\":" + String(slots[i].enabled ? "true" : "false") + ",\"t\":\"" + String(tBuf) + "\"}";
    if (i < FEED_SLOTS - 1) json += ",";
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void handleApiFeed() {
  if (server.method() == HTTP_POST || server.method() == HTTP_GET) {
    if (!feeding) startFeed("手动");
    server.send(200, "application/json", "{\"status\":\"ok\",\"feeding\":true}");
  } else {
    server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
  }
}

void handleApiStop() {
  if (server.method() == HTTP_POST || server.method() == HTTP_GET) {
    stopFeed();
    server.send(200, "application/json", "{\"status\":\"ok\",\"feeding\":false}");
  } else {
    server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
  }
}

void handleFeedNow() {
  if (!feeding) startFeed("手动");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleStopNow() {
  stopFeed();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleConfig() {
  conf.handleFormRequest(&server);
}

// ---------------- 网络初始化与自愈维护 ----------------
void initNetwork() {
  WiFi.hostname(conf.getApName());
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  String wifiSsid = conf.getString("ssid");
  String wifiPwd = conf.getString("pwd");

  if (wifiSsid.length() == 0) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(conf.getApName());
    Serial.println(F("[WiFi] 未配置 WiFi，已开启配置热点:"));
    Serial.println(conf.getApName());
    Serial.print(F("热点 IP: "));
    Serial.println(WiFi.softAPIP());
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.begin(wifiSsid.c_str(), wifiPwd.c_str());
    Serial.print(F("[WiFi] 正在连接: "));
    Serial.println(wifiSsid);

    unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 12000) {
      delay(300);
      Serial.print(F("."));
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print(F("[WiFi] 连接成功！IP: "));
      Serial.println(WiFi.localIP());
    } else {
      Serial.println(F("[WiFi] STA 连接超时，进入 AP+STA 双模容灾..."));
      // 开启热点保证能连入配置，同时保留后台重连机制
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(conf.getApName());
      Serial.print(F("应急热点: "));
      Serial.println(conf.getApName());
      Serial.print(F("热点 IP: "));
      Serial.println(WiFi.softAPIP());
    }
  }
}

void maintainNetwork() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck >= 20000) {
    lastCheck = now;
    String wifiSsid = conf.getString("ssid");
    if (wifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
      Serial.println(F("[WiFi] 后台自动重连 STA..."));
      WiFi.reconnect();
    }
  }
}

void maintainNTP() {
  static unsigned long lastCheck = 0;
  static uint8_t ntpServerIndex = 0;
  static const char* ntpServers[] = {"ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org"};

  timeClient.update();

  // 若已联网但超过 25 秒仍未同步成功，轮换尝试下一组校时服务器
  if (WiFi.status() == WL_CONNECTED && !isTimeValid()) {
    if (millis() - lastCheck > 25000) {
      lastCheck = millis();
      ntpServerIndex = (ntpServerIndex + 1) % 3;
      Serial.print(F("[NTP] 尝试轮换校时服务器: "));
      Serial.println(ntpServers[ntpServerIndex]);
      timeClient.setPoolServerName(ntpServers[ntpServerIndex]);
      timeClient.forceUpdate();
    }
  }
}

void checkSchedule() {
  if (!isTimeValid()) return;

  int nowHour = timeClient.getHours();
  int nowMin = timeClient.getMinutes();
  int nowMinuteOfDay = nowHour * 60 + nowMin;

  if (nowMinuteOfDay != lastTriggerMinute) {
    for (int i = 0; i < FEED_SLOTS; i++) {
      if (slots[i].enabled && slots[i].hh == nowHour && slots[i].mm == nowMin) {
        lastTriggerMinute = nowMinuteOfDay;
        startFeed("定时");
        break;
      }
    }
  }
}

// ---------------- 主程序入口 ----------------
void setup() {
  // 1. 最先初始化继电器引脚并强制断开，有效抑制上电瞬间误跳变
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();
#if USE_LED_INDICATOR
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // 默认熄灭
#endif

  // 2. 串口初始化
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("===================================="));
  Serial.println(F("  AutoWaterFeed v2.0 for ESP-01S    "));
  Serial.println(F("===================================="));

  // 3. 读取并解析配置
  conf.setDescription(params);
  conf.readConfig();
  conf.registerOnSave(onConfigSaved);
  parseFeedConfig();

  // 4. 网络初始化与容灾启动
  initNetwork();

  // 5. NTP 校时初始化
  timeClient.begin();
  timeClient.setTimeOffset((int)conf.getInt("timezone") * 3600);
  timeClient.update();

  // 6. mDNS 服务 (<apName>.local)
  if (MDNS.begin(conf.getApName())) {
    Serial.printf("[mDNS] 可通过 http://%s.local 访问\n", conf.getApName());
    MDNS.addService("http", "tcp", 80);
  }

  // 7. Web 服务路由配置
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/feed", HTTP_ANY, handleApiFeed);
  server.on("/api/stop", HTTP_ANY, handleApiStop);
  server.on("/feed", HTTP_ANY, handleFeedNow);
  server.on("/stop", HTTP_ANY, handleStopNow);
  server.on("/config", HTTP_ANY, handleConfig);
  server.begin();
  Serial.println(F("[HTTP] Web 服务已启动 (端口 80)"));
}

void loop() {
  // 1. 维护后台重连与时钟
  maintainNetwork();
  maintainNTP();

  // 2. 喂水计时与防溢水硬限制检查
  if (feeding) {
    unsigned long nowMs = millis();
    // 达到预设出水时间 或 触发硬件安全最长时间强制切断
    if ((long)(nowMs - feedUntil) >= 0 || (nowMs - feedStartTime) > ((unsigned long)HARD_MAX_FEED_SECONDS * 1000UL)) {
      stopFeed();
    }
  } else {
    // 待机状态：按计划时间触发
    checkSchedule();
  }

  // 3. Web 请求与 mDNS 轮询
  server.handleClient();
  MDNS.update();
}
