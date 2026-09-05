/*
 * autowaterfeed - 猫用/宠物智能自动喂水器（ESP8266 / ESP-01S 专属优化版）
 *
 * 架构设计原则：
 *   1. 热点模式（AP 模式 / 容灾配网）：
 *      - 单一职责：仅能配网，呈现专用的极简配网页面，支持强制门户 (Captive Portal)。
 *      - 严格禁止并剥离任何喂水操作与调度设置，无干扰无冗余。
 *   2. 正常工作模式（STA 模式 / 联网后）：
 *      - 单个页面集成所有功能（All-in-One Dashboard）：
 *        ① 实时设备状态（时间、倒计时、上次出水、下次计划、WiFi信号、运行时间）
 *        ② 核心动作控制（立即出水、紧急停止）
 *        ③ 计划与参数设置（4 个时段开启/时间、单次出水时长、时区）直接就地修改并无刷新保存
 *        ④ 网络维护选项（一键重置配网）
 *   3. 硬件安全与极速响应：
 *      - ESP-01S 专属 GPIO 0 继电器控制 + GPIO 2 状态指示灯
 *      - 上电防误触锁定 + 180s 硬件硬超时防溢水看门狗
 *      - HTML/CSS/JS 全量静态存入 Flash (PROGMEM)，运行时 0 堆内存占用
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>

#include <WiFiUdp.h>
#include <NTPClient.h>

#include <WebConfig.h>

// ---------------- 硬件配置 ----------------
// ESP-01S 继电器模块走线固定为 GPIO 0 (IO0)
#define RELAY_PIN 0

// 市面主流 ESP-01S 继电器底座为低电平触发：
// 若模块为高电平触发，将下面两行互换
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

// DNS Captive Portal 端口
const byte DNS_PORT = 53;

// ---------------- 全局对象 ----------------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "ntp.aliyun.com", 28800, 60000);

ESP8266WebServer server(80);
DNSServer dnsServer;
WebConfig conf;

// ---------------- 喂水调度与状态 ----------------
struct FeedSlot {
  bool enabled;
  int hh;
  int mm;
};

FeedSlot slots[FEED_SLOTS];
int feedDurationS = 10;            // 每次喂水时长（秒）
int timezoneOffsetH = 8;           // 时区偏移（小时，默认北京时间 UTC+8）
bool feeding = false;              // 是否正在出水
unsigned long feedStartTime = 0;   // 本次喂水开始时刻（millis）
unsigned long feedUntil = 0;       // 本次喂水结束时刻（millis）
int lastTriggerMinute = -1;        // 当天上次触发喂水的分钟数（0~1439）
String lastFeedInfo = "暂无记录";   // 上次喂水记录信息

// ---------------- Web 配置项描述（保持向后兼容） ----------------
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

// ---------------- 页面 1：热点模式专属配网页面（单一职责，PROGMEM） ----------------
const char AP_SETUP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>喂水器无线配网</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Microsoft YaHei", sans-serif;
  background: #f0f4f8;
  color: #1e293b;
  min-height: 100vh;
  padding: 20px 16px;
  display: flex;
  justify-content: center;
  align-items: center;
}
.box {
  width: 100%;
  max-width: 380px;
  background: #ffffff;
  border-radius: 20px;
  padding: 24px 20px;
  box-shadow: 0 10px 25px rgba(0, 0, 0, 0.06);
}
.header { text-align: center; margin-bottom: 20px; }
.header h1 { font-size: 20px; font-weight: 700; color: #0f172a; margin-bottom: 6px; }
.header p { font-size: 13px; color: #64748b; line-height: 1.4; }
.form-group { margin-bottom: 16px; }
label { display: block; font-size: 13px; font-weight: 600; color: #334155; margin-bottom: 6px; }
.input-wrap { position: relative; }
input, select {
  width: 100%;
  padding: 12px 14px;
  border: 1.5px solid #cbd5e1;
  border-radius: 12px;
  font-size: 15px;
  color: #0f172a;
  background: #f8fafc;
  outline: none;
  transition: all 0.2s;
}
input:focus, select:focus { border-color: #2563eb; background: #fff; box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.15); }
.scan-btn {
  display: inline-block;
  margin-top: 6px;
  font-size: 12px;
  color: #2563eb;
  cursor: pointer;
  font-weight: 600;
}
.btn {
  width: 100%;
  padding: 14px;
  border: none;
  border-radius: 12px;
  font-size: 16px;
  font-weight: 700;
  color: #fff;
  background: linear-gradient(135deg, #2563eb, #1d4ed8);
  cursor: pointer;
  box-shadow: 0 4px 14px rgba(37, 99, 235, 0.35);
  transition: transform 0.1s;
}
.btn:active { transform: scale(0.98); }
.btn:disabled { background: #94a3b8; box-shadow: none; cursor: not-allowed; }
.hint-card {
  margin-top: 18px;
  padding: 12px;
  background: #f1f5f9;
  border-radius: 12px;
  font-size: 12px;
  color: #64748b;
  line-height: 1.5;
}
.hint-card b { color: #334155; }
#statusMsg {
  display: none;
  padding: 12px;
  border-radius: 10px;
  font-size: 13px;
  margin-top: 14px;
  text-align: center;
  line-height: 1.5;
}
.msg-success { background: #dcfce7; color: #166534; border: 1px solid #bbf7d0; }
.msg-error { background: #fee2e2; color: #991b1b; border: 1px solid #fecaca; }
</style>
</head>
<body>
<div class="box">
  <div class="header">
    <h1>📶 喂水器 WiFi 配网</h1>
    <p>仅限连接 2.4GHz 无线网络。<br>配置完成后设备将自动联网。</p>
  </div>

  <div class="form-group">
    <label for="ssidSelect">选择周边网络 (可选)</label>
    <select id="ssidSelect" onchange="onSelectSsid()">
      <option value="">-- 点击下方扫描或直接输入 --</option>
    </select>
    <div class="scan-btn" onclick="startScan()">🔍 扫描周围 WiFi 信号</div>
  </div>

  <div class="form-group">
    <label for="ssidInput">WiFi 名称 (SSID)</label>
    <input type="text" id="ssidInput" placeholder="请输入路由器 WiFi 名称" required>
  </div>

  <div class="form-group">
    <label for="pwdInput">WiFi 密码</label>
    <input type="password" id="pwdInput" placeholder="请输入 WiFi 密码 (无密码留空)">
  </div>

  <button class="btn" id="submitBtn" onclick="submitWifi()">连接家庭网络</button>
  <div id="statusMsg"></div>

  <div class="hint-card">
    <b>💡 配网须知：</b><br>
    1. 请确保路由器开启了 <b>2.4GHz</b> 频段（ESP8266 不支持 5GHz）。<br>
    2. 点击连接后设备将重启并接入家庭网络，届时手机切回原网络即可打开控制面板。
  </div>
</div>

<script>
async function startScan() {
  const sel = document.getElementById('ssidSelect');
  sel.innerHTML = '<option value="">正在扫描附近网络...</option>';
  try {
    const res = await fetch('/api/scan');
    const data = await res.json();
    if (data.networks && data.networks.length > 0) {
      let opt = '<option value="">-- 请选择扫描到的 WiFi --</option>';
      data.networks.forEach(n => {
        if (n.ssid) {
          opt += `<option value="${n.ssid}">${n.ssid} (${n.rssi} dBm)</option>`;
        }
      });
      sel.innerHTML = opt;
    } else {
      sel.innerHTML = '<option value="">未找到网络，请重试或手动输入</option>';
    }
  } catch (e) {
    sel.innerHTML = '<option value="">扫描失败，请手动输入 SSID</option>';
  }
}

function onSelectSsid() {
  const sel = document.getElementById('ssidSelect');
  if (sel.value) {
    document.getElementById('ssidInput').value = sel.value;
    document.getElementById('pwdInput').focus();
  }
}

async function submitWifi() {
  const ssid = document.getElementById('ssidInput').value.trim();
  const pwd = document.getElementById('pwdInput').value;
  const btn = document.getElementById('submitBtn');
  const msg = document.getElementById('statusMsg');

  if (!ssid) {
    alert('请输入 WiFi 名称 (SSID)');
    return;
  }

  btn.disabled = true;
  btn.innerText = '正在保存配置...';
  msg.style.display = 'none';

  const body = new URLSearchParams();
  body.append('ssid', ssid);
  body.append('pwd', pwd);

  try {
    const res = await fetch('/api/save_wifi', {
      method: 'POST',
      body: body
    });
    const d = await res.json();
    if (d.status === 'ok') {
      msg.className = 'msg-success';
      msg.innerHTML = `<b>✓ WiFi 保存成功！</b><br>设备正在重启连入「${ssid}」...<br>请将手机切回家庭 WiFi，随后通过设备 IP 或 mDNS 访问主页。`;
      msg.style.display = 'block';
      btn.style.display = 'none';
    } else {
      throw new Error(d.msg || '保存失败');
    }
  } catch (e) {
    msg.className = 'msg-error';
    msg.innerText = e.message || '网络通讯异常，请重试';
    msg.style.display = 'block';
    btn.disabled = false;
    btn.innerText = '连接家庭网络';
  }
}

window.onload = () => {
  startScan();
};
</script>
</body>
</html>
)rawliteral";

// ---------------- 页面 2：正常工作模式全功能单页控制面板（PROGMEM） ----------------
const char NORMAL_DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>智能宠物喂水器</title>
<style>
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Microsoft YaHei", sans-serif;
  background: #f1f5f9;
  color: #1e293b;
  min-height: 100vh;
  padding: 16px;
  display: flex;
  justify-content: center;
}
.app { width: 100%; max-width: 420px; }
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
.section-title {
  font-size: 14px;
  font-weight: 700;
  color: #334155;
  margin-bottom: 12px;
  display: flex;
  justify-content: space-between;
  align-items: center;
}
.slots-container { display: flex; flex-direction: column; gap: 8px; margin-bottom: 12px; }
.slot-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  background: #f8fafc;
  padding: 10px 12px;
  border-radius: 12px;
}
.slot-label { font-size: 13px; font-weight: 600; color: #334155; min-width: 60px; }
.slot-controls { display: flex; align-items: center; gap: 10px; }
.slot-time-input {
  padding: 6px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 8px;
  font-size: 14px;
  background: #fff;
  outline: none;
}
.switch { position: relative; display: inline-block; width: 44px; height: 24px; }
.switch input { opacity: 0; width: 0; height: 0; }
.slider {
  position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0;
  background-color: #cbd5e1;
  transition: .2s;
  border-radius: 24px;
}
.slider:before {
  position: absolute; content: ""; height: 18px; width: 18px; left: 3px; bottom: 3px;
  background-color: white;
  transition: .2s;
  border-radius: 50%;
}
input:checked + .slider { background-color: #10b981; }
input:checked + .slider:before { transform: translateX(20px); }
.param-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 4px;
  font-size: 13px;
  color: #334155;
}
.param-input {
  width: 90px;
  padding: 6px 8px;
  border: 1px solid #cbd5e1;
  border-radius: 8px;
  font-size: 14px;
  text-align: center;
}
.btn-save {
  width: 100%;
  padding: 12px;
  border: none;
  border-radius: 12px;
  background: #2563eb;
  color: #fff;
  font-size: 15px;
  font-weight: 600;
  cursor: pointer;
  margin-top: 8px;
  transition: background 0.2s;
}
.btn-save:active { background: #1d4ed8; }
.toast {
  position: fixed;
  bottom: 24px;
  left: 50%;
  transform: translateX(-50%);
  background: rgba(15, 23, 42, 0.9);
  color: #fff;
  padding: 10px 18px;
  border-radius: 30px;
  font-size: 13px;
  font-weight: 500;
  display: none;
  z-index: 100;
  box-shadow: 0 4px 12px rgba(0,0,0,0.15);
}
.reset-zone {
  text-align: center;
  margin-top: 14px;
}
.reset-link {
  font-size: 12px;
  color: #94a3b8;
  text-decoration: underline;
  cursor: pointer;
}
.footer { text-align: center; font-size: 11px; color: #94a3b8; margin-top: 10px; }
</style>
</head>
<body>
<div class="app">
  <div class="header">
    <h1>🐱 智能宠物喂水器</h1>
    <p>ESP8266 Auto Feeder</p>
  </div>

  <!-- 1. 实时出水与核心操作 -->
  <div class="card">
    <div class="status-header">
      <span id="statusBadge" class="badge badge-idle">● 待机就绪</span>
      <span id="curTime" class="clock">--:--:--</span>
    </div>
    <div class="main-display">
      <div id="mainState" class="main-state">当前状态</div>
      <div id="countdownDisplay" class="countdown">待机中</div>
    </div>
    <button id="actionBtn" class="btn btn-feed" onclick="handleAction()">💧 立即出水</button>
  </div>

  <!-- 2. 状态与监控指标 -->
  <div class="card">
    <div class="section-title">📊 实时监控</div>
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
        <div id="feedDurDisplay" class="info-val">10 秒</div>
      </div>
      <div class="info-item">
        <div class="info-label">系统运行</div>
        <div id="uptime" class="info-val">--</div>
      </div>
      <div class="info-item">
        <div class="info-label">当前网络</div>
        <div id="wifi" class="info-val">--</div>
      </div>
      <div class="info-item">
        <div class="info-label">局域网 IP</div>
        <div id="ip" class="info-val">--</div>
      </div>
    </div>
  </div>

  <!-- 3. 定时计划与系统参数设置（就地修改保存，单一页面搞定一切） -->
  <div class="card">
    <div class="section-title">
      <span>⏰ 定时计划与出水设置</span>
    </div>

    <div class="slots-container">
      <div class="slot-row">
        <span class="slot-label">时段 1</span>
        <div class="slot-controls">
          <input type="time" id="slotTime_0" class="slot-time-input">
          <label class="switch"><input type="checkbox" id="slotEn_0"><span class="slider"></span></label>
        </div>
      </div>
      <div class="slot-row">
        <span class="slot-label">时段 2</span>
        <div class="slot-controls">
          <input type="time" id="slotTime_1" class="slot-time-input">
          <label class="switch"><input type="checkbox" id="slotEn_1"><span class="slider"></span></label>
        </div>
      </div>
      <div class="slot-row">
        <span class="slot-label">时段 3</span>
        <div class="slot-controls">
          <input type="time" id="slotTime_2" class="slot-time-input">
          <label class="switch"><input type="checkbox" id="slotEn_2"><span class="slider"></span></label>
        </div>
      </div>
      <div class="slot-row">
        <span class="slot-label">时段 4</span>
        <div class="slot-controls">
          <input type="time" id="slotTime_3" class="slot-time-input">
          <label class="switch"><input type="checkbox" id="slotEn_3"><span class="slider"></span></label>
        </div>
      </div>
    </div>

    <div class="param-row">
      <span>单次喂水时长 (秒, 1~180):</span>
      <input type="number" id="cfgDuration" class="param-input" min="1" max="180">
    </div>
    <div class="param-row">
      <span>UTC 时区 (北京时间为 8):</span>
      <input type="number" id="cfgTimezone" class="param-input" min="-12" max="14">
    </div>

    <button id="saveBtn" class="btn-save" onclick="saveSettings()">💾 保存计划与参数</button>
  </div>

  <!-- 4. 辅助维护 -->
  <div class="reset-zone">
    <span class="reset-link" onclick="confirmResetWifi()">重新配置 WiFi 网络</span>
  </div>
  <div class="footer">AutoWaterFeed v2.1 · All-in-One Dashboard</div>
</div>

<div id="toast" class="toast"></div>

<script>
let isFeeding = false;
let polling = false;
let formInitialized = false;

function showToast(text) {
  const t = document.getElementById('toast');
  t.innerText = text;
  t.style.display = 'block';
  setTimeout(() => { t.style.display = 'none'; }, 2500);
}

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
  document.getElementById('feedDurDisplay').innerText = d.duration + ' 秒';
  document.getElementById('uptime').innerText = d.uptime;
  document.getElementById('ip').innerText = d.ip;
  document.getElementById('wifi').innerText = d.wifi ? `${d.wifi} (${d.rssi}dBm)` : '--';

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
    actionBtn.innerText = '💧 立即出水 (' + d.duration + 's)';
  }

  // 仅在首次拉取时填充表单输入框，防止用户编辑时被轮询冲掉
  if (!formInitialized) {
    formInitialized = true;
    document.getElementById('cfgDuration').value = d.duration || 10;
    document.getElementById('cfgTimezone').value = d.timezone !== undefined ? d.timezone : 8;

    if (d.slots && d.slots.length) {
      for (let i = 0; i < d.slots.length; i++) {
        const timeInput = document.getElementById('slotTime_' + i);
        const enInput = document.getElementById('slotEn_' + i);
        if (timeInput) timeInput.value = (d.slots[i].t === '--:--') ? '08:00' : d.slots[i].t;
        if (enInput) enInput.checked = d.slots[i].en;
      }
    }
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

async function saveSettings() {
  const btn = document.getElementById('saveBtn');
  btn.disabled = true;
  btn.innerText = '正在保存...';

  const body = new URLSearchParams();
  body.append('feedduration', document.getElementById('cfgDuration').value);
  body.append('timezone', document.getElementById('cfgTimezone').value);

  for (let i = 0; i < 4; i++) {
    const en = document.getElementById('slotEn_' + i).checked ? '1' : '0';
    const t = document.getElementById('slotTime_' + i).value;
    body.append('feed' + (i + 1) + 'en', en);
    body.append('feed' + (i + 1) + 't', t);
  }

  try {
    const res = await fetch('/api/save_settings', {
      method: 'POST',
      body: body
    });
    const d = await res.json();
    showToast(d.msg || '设置已保存并生效！');
    fetchStatus();
  } catch (e) {
    showToast('保存失败，请检查连接');
  } finally {
    btn.disabled = false;
    btn.innerText = '💾 保存计划与参数';
  }
}

async function confirmResetWifi() {
  if (confirm('确定要清除当前 WiFi 配置并重启进入配网热点模式吗？')) {
    try {
      await fetch('/api/reset_wifi', { method: 'POST' });
      showToast('设备正在重启进入配网热点...');
      setTimeout(() => { alert('设备已重启，请重新连接 ESP 热点进行配网。'); }, 1500);
    } catch(e) {
      showToast('重置失败');
    }
  }
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

  if (!anyEnabled) return "未启用任何计划";

  char buf[32];
  if (minToday != 9999) {
    snprintf(buf, sizeof(buf), "今日 %02d:%02d", minToday / 60, minToday % 60);
  } else if (minTomorrow != 9999) {
    snprintf(buf, sizeof(buf), "次日 %02d:%02d", minTomorrow / 60, minTomorrow % 60);
  } else {
    return "今日无即将执行计划";
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

  timezoneOffsetH = conf.getInt("timezone");
  if (timezoneOffsetH < -12 || timezoneOffsetH > 14) timezoneOffsetH = 8;
  timeClient.setTimeOffset(timezoneOffsetH * 3600);
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

  String tStr = isTimeValid() ? timeClient.getFormattedTime() : "未校时";
  lastFeedInfo = tStr + " (" + String(source) + ", " + String(feedDurationS) + "s)";
  Serial.printf("[feed] START %ds [%s] @ %s\n", feedDurationS, source, tStr.c_str());
}

void stopFeed() {
  if (!feeding) return;
  feeding = false;
  relayOff();
  Serial.printf("[feed] STOP @ %s\n", isTimeValid() ? timeClient.getFormattedTime().c_str() : "未校时");
}

// ---------------- Web 路由与服务接口 ----------------

// 根路径分流：AP 模式只呈现配网页面（单一职责）；正常模式呈现全功能单页控制台
void handleRoot() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send_P(200, "text/html", AP_SETUP_HTML);
  } else {
    server.send_P(200, "text/html", NORMAL_DASHBOARD_HTML);
  }
}

// Captive Portal 劫持重定向（iOS / Android / Windows 探针）
void handleCaptivePortal() {
  if (WiFi.status() != WL_CONNECTED) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send_P(200, "text/html", NORMAL_DASHBOARD_HTML);
  }
}

void handleNotFound() {
  if (WiFi.status() != WL_CONNECTED) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not Found");
  }
}

// [AP 模式 API] 扫描周边 WiFi 列表
void handleApiScan() {
  int n = WiFi.scanNetworks();
  String json = "{\"status\":\"done\",\"networks\":[";
  for (int i = 0; i < n; ++i) {
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    if (i < n - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// [AP 模式 API] 保存配网信息并重启
void handleApiSaveWifi() {
  String ssid = server.arg("ssid");
  String pwd = server.arg("pwd");
  ssid.trim();
  pwd.trim();

  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"SSID不能为空\"}");
    return;
  }

  conf.setValue("ssid", ssid);
  conf.setValue("pwd", pwd);
  conf.writeConfig();

  server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"WiFi配置已保存，正在重启...\"}");
  delay(1000);
  ESP.restart();
}

// [正常模式 API] 获取综合运行状态与各计划数据
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
  json += "\"timezone\":" + String(timezoneOffsetH) + ",";
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

// [正常模式 API] 立即出水
void handleApiFeed() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(403, "application/json", "{\"error\":\"AP模式不可用\"}");
    return;
  }
  if (!feeding) startFeed("手动");
  server.send(200, "application/json", "{\"status\":\"ok\",\"feeding\":true}");
}

// [正常模式 API] 立即停止出水
void handleApiStop() {
  stopFeed();
  server.send(200, "application/json", "{\"status\":\"ok\",\"feeding\":false}");
}

// [正常模式 API] 保存所有计划和全局参数（就地保存生效）
void handleApiSaveSettings() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(403, "application/json", "{\"error\":\"AP模式不可用\"}");
    return;
  }

  if (server.hasArg("feedduration")) {
    int dur = constrain(server.arg("feedduration").toInt(), 1, MAX_DURATION_S);
    conf.setValue("feedduration", String(dur));
  }
  if (server.hasArg("timezone")) {
    int tz = constrain(server.arg("timezone").toInt(), -12, 14);
    conf.setValue("timezone", String(tz));
  }

  for (int i = 0; i < FEED_SLOTS; i++) {
    char nameEn[16], nameT[16];
    snprintf(nameEn, sizeof(nameEn), "feed%den", i + 1);
    snprintf(nameT, sizeof(nameT), "feed%dt", i + 1);
    if (server.hasArg(nameEn)) conf.setValue(nameEn, server.arg(nameEn));
    if (server.hasArg(nameT)) conf.setValue(nameT, server.arg(nameT));
  }

  conf.writeConfig();
  parseFeedConfig();

  server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"设置已保存并立即生效\"}");
}

// [正常模式 API] 重置 WiFi 配置并重启进入 AP
void handleApiResetWifi() {
  conf.setValue("ssid", "");
  conf.setValue("pwd", "");
  conf.writeConfig();
  server.send(200, "application/json", "{\"status\":\"ok\",\"msg\":\"WiFi 已重置，正在重启进入热点...\"}");
  delay(1000);
  ESP.restart();
}

// ---------------- 网络初始化与模式分流 ----------------
void startAPMode() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(conf.getApName());
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println(F("[WiFi] 进入热点配网模式:"));
  Serial.print(F("  热点名称: ")); Serial.println(conf.getApName());
  Serial.print(F("  配网地址: http://")); Serial.println(WiFi.softAPIP());
}

void initNetwork() {
  WiFi.hostname(conf.getApName());
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  String wifiSsid = conf.getString("ssid");
  String wifiPwd = conf.getString("pwd");

  if (wifiSsid.length() == 0) {
    // 首次使用或未配置 WiFi：直接进入配网模式
    startAPMode();
  } else {
    // 已有配置，尝试连接家庭 WiFi
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
      Serial.println(F("[WiFi] STA 连接超时，进入热点容灾模式..."));
      startAPMode();
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
      Serial.println(F("[WiFi] 后台自动尝试重连 STA..."));
      WiFi.reconnect();
    }
  }
}

void maintainNTP() {
  if (WiFi.status() != WL_CONNECTED) return; // AP 模式不执行 NTP 维护

  static unsigned long lastCheck = 0;
  static uint8_t ntpServerIndex = 0;
  static const char* ntpServers[] = {"ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org"};

  timeClient.update();

  if (!isTimeValid()) {
    if (millis() - lastCheck > 25000) {
      lastCheck = millis();
      ntpServerIndex = (ntpServerIndex + 1) % 3;
      Serial.print(F("[NTP] 轮换校时服务器: "));
      Serial.println(ntpServers[ntpServerIndex]);
      timeClient.setPoolServerName(ntpServers[ntpServerIndex]);
      timeClient.forceUpdate();
    }
  }
}

void checkSchedule() {
  if (WiFi.status() != WL_CONNECTED || !isTimeValid()) return; // 仅联网校时后执行计划

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
  // 1. 最先初始化继电器引脚并强制置为断开，彻底杜绝开机误出水
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();
#if USE_LED_INDICATOR
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
#endif

  // 2. 串口初始化
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("=========================================="));
  Serial.println(F("  AutoWaterFeed v2.1 (AP/Normal Separated)"));
  Serial.println(F("=========================================="));

  // 3. 读取并解析配置
  conf.setDescription(params);
  conf.readConfig();
  parseFeedConfig();

  // 4. 网络初始化
  initNetwork();

  // 5. NTP 校时初始化
  timeClient.begin();
  timeClient.setTimeOffset(timezoneOffsetH * 3600);
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }

  // 6. mDNS 服务
  if (MDNS.begin(conf.getApName())) {
    Serial.printf("[mDNS] 可通过 http://%s.local 访问\n", conf.getApName());
    MDNS.addService("http", "tcp", 80);
  }

  // 7. Web 服务路由配置
  server.on("/", HTTP_GET, handleRoot);

  // Captive Portal 探针重定向路由
  server.on("/generate_204", handleCaptivePortal);
  server.on("/gen_204", handleCaptivePortal);
  server.on("/hotspot-detect.html", handleCaptivePortal);
  server.on("/ncsi.txt", handleCaptivePortal);
  server.on("/connecttest.txt", handleCaptivePortal);
  server.on("/redirect", handleCaptivePortal);

  // AP 专属 API
  server.on("/api/scan", HTTP_GET, handleApiScan);
  server.on("/api/save_wifi", HTTP_POST, handleApiSaveWifi);

  // 正常模式 API
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/feed", HTTP_POST, handleApiFeed);
  server.on("/api/stop", HTTP_POST, handleApiStop);
  server.on("/api/save_settings", HTTP_POST, handleApiSaveSettings);
  server.on("/api/reset_wifi", HTTP_POST, handleApiResetWifi);

  // 兼容老路径：/config 和 /feed 统一重定向到 /
  server.on("/config", HTTP_ANY, []() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/feed", HTTP_ANY, []() {
    if (WiFi.status() == WL_CONNECTED && !feeding) startFeed("手动");
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  server.on("/stop", HTTP_ANY, []() {
    stopFeed();
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println(F("[HTTP] Web 服务已启动 (端口 80)"));
}

void loop() {
  // 1. AP 模式处理 DNS 劫持
  if (WiFi.status() != WL_CONNECTED) {
    dnsServer.processNextRequest();
  }

  // 2. 维护后台重连与网络时钟
  maintainNetwork();
  maintainNTP();

  // 3. 喂水计时与硬件防溢水硬限制检查
  if (feeding) {
    unsigned long nowMs = millis();
    if ((long)(nowMs - feedUntil) >= 0 || (nowMs - feedStartTime) > ((unsigned long)HARD_MAX_FEED_SECONDS * 1000UL)) {
      stopFeed();
    }
  } else {
    // 待机状态：仅在正常联网时检查计划调度
    checkSchedule();
  }

  // 4. Web 请求与 mDNS 轮询
  server.handleClient();
  MDNS.update();
}
