#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>

// --- 1. DEFAULT FALLBACK CREDENTIALS ---
const char* fallback_ssid = "YOUR_WIFI_NAME";
const char* fallback_pass = "YOUR_WIFI_PASSWORD";
const char* fallback_user = "admin";
const char* fallback_www_pass = "diwali2026";

// Persistent Config Structure stored in EEPROM
struct Config {
  uint32_t magic;
  char wifi_ssid[33];
  char wifi_pass[65];
  char www_user[33];
  char www_pass[33];
} config;

const uint32_t CONFIG_MAGIC = 0xD17A2026;

// --- 2. BRUTE-FORCE PROTECTION STATE ---
int failedAttempts = 0;
unsigned long lockoutUntil = 0;
const int MAX_ATTEMPTS = 5;
const unsigned long LOCKOUT_DURATION = 180000; // 3 minutes (180,000 ms)

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// --- 3. HARDWARE PINS & STATE VARIABLES ---
const int relays[4] = {5, 4, 14, 12}; // D1, D2, D5, D6
const int flashBtnPin = 0;            // D3 (GPIO 0) onboard FLASH button

#define RELAY_ON LOW
#define RELAY_OFF HIGH

int currentMode = 10;          // Default: Auto-Cycle DJ Mode
int activePattern = 1;
unsigned long stepDelay = 600; // Speed in ms
unsigned long lastStepTime = 0;
unsigned long lastAutoSwitch = 0;
int stepIndex = 0;
bool isAPMode = false;
bool currentRelayState[4] = {0, 0, 0, 0};

// Shared Footer HTML with Copyright & GitHub Backlink
const char FOOTER_SNIPPET[] = "<div style='margin-top:24px;padding-top:14px;border-top:1px solid rgba(255,209,102,0.22);font-size:12px;color:#ccc;text-align:center;'>&copy; 2026 &bull; Created with &#10084;&#65039; by <a href='https://github.com/rathindrabera' target='_blank' rel='noopener noreferrer' style='color:#ffd166;text-decoration:none;font-weight:bold;'>Rathindra Bera</a></div>";

// --- 4. VERTICAL PATTERN LOOKUP TABLES ---
const bool wave[4][4] = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}};
const bool darkWave[4][4] = {{0,1,1,1}, {1,0,1,1}, {1,1,0,1}, {1,1,1,0}};
const bool pingPong[6][4] = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}, {0,0,1,0}, {0,1,0,0}};
const bool curtain[4][4] = {{0,0,0,0}, {0,1,1,0}, {1,1,1,1}, {1,0,0,1}};
const bool ribbon[4][4] = {{1,1,0,0}, {0,1,1,0}, {0,0,1,1}, {1,0,0,1}};
const bool fillWipe[8][4] = {{1,0,0,0}, {1,1,0,0}, {1,1,1,0}, {1,1,1,1}, {0,1,1,1}, {0,0,1,1}, {0,0,0,1}, {0,0,0,0}};
const bool oddEven[2][4] = {{1,0,1,0}, {0,1,0,1}};

// --- 5. EEPROM CONFIGURATION HELPERS ---
void saveConfig() {
  config.magic = CONFIG_MAGIC;
  EEPROM.put(0, config);
  EEPROM.commit();
}

void resetToDefaults() {
  memset(&config, 0, sizeof(Config));
  strncpy(config.wifi_ssid, fallback_ssid, sizeof(config.wifi_ssid) - 1);
  strncpy(config.wifi_pass, fallback_pass, sizeof(config.wifi_pass) - 1);
  strncpy(config.www_user, fallback_user, sizeof(config.www_user) - 1);
  strncpy(config.www_pass, fallback_www_pass, sizeof(config.www_pass) - 1);
  saveConfig();
}

void loadConfig() {
  EEPROM.begin(512);
  EEPROM.get(0, config);
  if (config.magic != CONFIG_MAGIC) {
    resetToDefaults();
  }
}

// --- 6. BRUTE-FORCE PROTECTED AUTHENTICATION CHECK ---
bool checkAuth() {
  unsigned long now = millis();
  if (lockoutUntil > 0) {
    if (now < lockoutUntil) {
      unsigned long secsLeft = (lockoutUntil - now) / 1000;
      String lockMsg = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
      lockMsg += "<style>body{font-family:system-ui,sans-serif;background:#14051e;color:#fff;text-align:center;padding:40px 20px;}";
      lockMsg += ".box{background:#260b38;border:2px solid #e63946;padding:24px;border-radius:14px;max-width:400px;margin:0 auto;}</style></head><body>";
      lockMsg += "<div class='box'><h2 style='color:#e63946;'>&#128274; Security Lockout</h2>";
      lockMsg += "<p>Too many failed login attempts.</p>";
      lockMsg += "<p>Please wait <b>" + String(secsLeft) + " seconds</b> before trying again.</p></div>";
      lockMsg += FOOTER_SNIPPET;
      lockMsg += "</body></html>";
      server.send(429, "text/html", lockMsg);
      return false;
    } else {
      lockoutUntil = 0;
      failedAttempts = 0;
    }
  }

  if (!server.authenticate(config.www_user, config.www_pass)) {
    failedAttempts++;
    if (failedAttempts >= MAX_ATTEMPTS) {
      lockoutUntil = now + LOCKOUT_DURATION;
      String lockMsg = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'></head>";
      lockMsg += "<body style='font-family:sans-serif;background:#14051e;color:#fff;text-align:center;padding:40px;'>";
      lockMsg += "<h2 style='color:#e63946;'>&#128274; Locked Out for 3 Minutes</h2>";
      lockMsg += "<p>5 failed login attempts detected.</p>";
      lockMsg += FOOTER_SNIPPET;
      lockMsg += "</body></html>";
      server.send(429, "text/html", lockMsg);
      return false;
    }
    server.requestAuthentication();
    return false;
  }

  failedAttempts = 0;
  return true;
}

void setRelays(bool r1, bool r2, bool r3, bool r4) {
  currentRelayState[0] = r1;
  currentRelayState[1] = r2;
  currentRelayState[2] = r3;
  currentRelayState[3] = r4;
  digitalWrite(relays[0], r1 ? RELAY_ON : RELAY_OFF);
  digitalWrite(relays[1], r2 ? RELAY_ON : RELAY_OFF);
  digitalWrite(relays[2], r3 ? RELAY_ON : RELAY_OFF);
  digitalWrite(relays[3], r4 ? RELAY_ON : RELAY_OFF);
}

String getModeName(int m) {
  switch(m) {
    case 0: return "All OFF";
    case 1: return "1. Single Running Wave";
    case 2: return "2. Inverted Dark Wave";
    case 3: return "3. Knight Rider Ping-Pong";
    case 4: return "4. Theatre Curtain (Center-Out)";
    case 5: return "5. Rolling Thick Ribbon";
    case 6: return "6. Curtain Fill & Wipe";
    case 7: return "7. Odd / Even Dance";
    case 8: return "8. Random Sparkle";
    case 9: return "All Lights Steady ON";
    case 10: return "Auto-Cycle All 8 Patterns (30s each)";
    default: return "Unknown";
  }
}

// --- 7. PUBLIC DIWALI SETUP GUIDE PAGE (NO LOGIN REQUIRED) ---
const char GUIDE_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Diwali Smart Light Show - Setup & Wiring Guide</title>
<style>
  :root {
    --bg-deep: #14051e;
    --bg-card: #260b38;
    --gold: #ffd166;
    --saffron: #ff7b00;
    --crimson: #e63946;
    --text-light: #fefae0;
    --teal: #06d6a0;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    font-family: system-ui, -apple-system, sans-serif;
    background: radial-gradient(circle at top, #3b0d54 0%, var(--bg-deep) 80%);
    color: var(--text-light);
    line-height: 1.6;
    padding: 15px 12px 40px;
  }
  .container { max-width: 780px; margin: 0 auto; }
  .hero {
    text-align: center;
    padding: 24px 16px;
    background: linear-gradient(135deg, rgba(255,123,0,0.22), rgba(230,57,70,0.25));
    border: 2px solid var(--gold);
    border-radius: 16px;
    box-shadow: 0 0 20px rgba(255, 209, 102, 0.25);
    margin-bottom: 20px;
  }
  .diya-icon { font-size: 40px; }
  h1 { color: var(--gold); margin: 8px 0 4px; font-size: 24px; }
  .subtitle { color: #f4a261; font-size: 14px; margin: 0 0 14px; }
  .btn {
    display: inline-block;
    padding: 11px 20px;
    border-radius: 99px;
    font-weight: bold;
    text-decoration: none;
    color: #14051e;
    background: linear-gradient(90deg, var(--gold), var(--saffron));
    box-shadow: 0 4px 12px rgba(255, 123, 0, 0.4);
  }
  .card {
    background: var(--bg-card);
    border: 1px solid rgba(255, 209, 102, 0.35);
    border-radius: 14px;
    padding: 18px;
    margin-bottom: 16px;
  }
  h2 {
    color: var(--gold);
    border-bottom: 1px solid rgba(255, 209, 102, 0.3);
    padding-bottom: 6px;
    margin-top: 0;
    font-size: 18px;
  }
  table { width: 100%; border-collapse: collapse; margin-top: 8px; font-size: 13px; }
  th, td { padding: 8px 10px; border: 1px solid rgba(255, 209, 102, 0.25); text-align: left; }
  th { background: rgba(255, 123, 0, 0.25); color: var(--gold); }
  code { background: rgba(0,0,0,0.45); color: var(--teal); padding: 2px 5px; border-radius: 4px; }
  .footer {
    text-align: center;
    font-size: 13px;
    color: #ccc;
    margin-top: 24px;
    padding-top: 14px;
    border-top: 1px solid rgba(255, 209, 102, 0.25);
  }
  .footer a { color: var(--gold); text-decoration: none; font-weight: bold; }
  .footer a:hover { text-decoration: underline; color: var(--saffron); }
</style>
</head>
<body>
<div class="container">
  <div class="hero">
    <div class="diya-icon">&#129684; &#10024; &#127878;</div>
    <h1>Smart Diwali Light Show Guide</h1>
    <p class="subtitle">Public Info &amp; Setup Guide (No Login Required) &bull; ESP8266 + 4-Ch Relay + 20 LED Strings</p>
    <a class="btn" href="/">&#127899; Go to Control Dashboard (Login Required)</a>
  </div>
  <div class="card">
    <h2>&#128268; Pinout &amp; 12V DC Wiring Summary</h2>
    <table>
      <tr><th>ESP8266 / Power</th><th>Relay Pin</th><th>Connected Load / Function</th></tr>
      <tr><td><code>5V Supply +5V &amp; GND</code></td><td><code>VCC &amp; GND</code></td><td>5V DC power for NodeMCU (<code>VIN/VU</code>) &amp; Relay</td></tr>
      <tr><td><code>D1 (GPIO 5)</code></td><td><code>IN1</code></td><td>Ch 1: Strings #1, #5, #9, #13, #17</td></tr>
      <tr><td><code>D2 (GPIO 4)</code></td><td><code>IN2</code></td><td>Ch 2: Strings #2, #6, #10, #14, #18</td></tr>
      <tr><td><code>D5 (GPIO 14)</code></td><td><code>IN3</code></td><td>Ch 3: Strings #3, #7, #11, #15, #19</td></tr>
      <tr><td><code>D6 (GPIO 12)</code></td><td><code>IN4</code></td><td>Ch 4: Strings #4, #8, #12, #16, #20</td></tr>
      <tr><td><code>12V Supply (+12V)</code></td><td><code>COM1 - COM4</code></td><td>Daisy-chain +12V into all 4 Middle Screws</td></tr>
      <tr><td><code>Bundles 1 - 4 (+)</code></td><td><code>NO1 - NO4</code></td><td>Connect each bundle's Positive (+) to Right Screw</td></tr>
      <tr><td><code>12V Supply (-)</code></td><td><code>Direct to Lights</code></td><td>Connect all 4 bundles' Negative (-) to 12V GND</td></tr>
    </table>
  </div>
  <div class="card">
    <h2>&#128272; Security &amp; Recovery Info</h2>
    <p><strong>Live Visualizer:</strong> The Control Dashboard includes a live 20-string vertical LED curtain simulator so you can preview patterns from indoors.</p>
    <p><strong>Brute-Force Lockout:</strong> 5 wrong password attempts locks the login page for 3 minutes.</p>
    <p><strong>Wi-Fi Rescue Hotspot:</strong> If your home Wi-Fi name changes or is unreachable on boot, the ESP8266 automatically creates a hotspot named <code>Diwali-Setup-AP</code> (password: <code>diwali2026</code>) so you can log in at <code>http://192.168.4.1</code> and update your Wi-Fi credentials.</p>
    <p><strong>Factory Reset Button:</strong> Hold the physical <code>FLASH</code> button on the board for 5 seconds while running to reset Wi-Fi &amp; admin login back to defaults.</p>
  </div>
  <div class="footer">
    <div>&#129684; Subh Diwali! Enjoy Your Smart Light Show &#129684;</div>
    <div style="margin-top:6px;">&copy; 2026 &bull; Created with &#10084;&#65039; by <a href="https://github.com/rathindrabera" target="_blank" rel="noopener noreferrer">Rathindra Bera</a></div>
  </div>
</div>
</body>
</html>
)=====";

void handleGuide() {
  server.send_P(200, "text/html", GUIDE_HTML);
}

// --- 8. SECURE WEB DASHBOARD (WITH LIVE 20-STRING LED VISUALIZER & FOOTER) ---
void handleRoot() {
  if (!checkAuth()) return;

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Diwali Light Show</title>";
  html += "<style>";
  html += "body{font-family:system-ui,sans-serif;background:radial-gradient(circle at top,#3b0d54,#120d2b);color:#fff;text-align:center;margin:0;padding:15px 12px 30px;}";
  html += "h1{color:#ffb703;margin:4px 0 8px;font-size:22px;}";
  html += ".vis-card{background:#180b29;border:1px solid #ffb703;border-radius:14px;padding:12px 10px;max-width:420px;margin:0 auto 12px;box-shadow:0 0 20px rgba(255,183,3,0.18);}";
  html += ".vis-header{display:flex;justify-content:space-between;align-items:center;font-size:12px;color:#ffd166;margin-bottom:8px;padding:0 4px;}";
  html += ".layout-toggle{background:#2d1950;color:#fff;border:1px solid #ffd166;border-radius:6px;padding:3px 7px;font-size:11px;cursor:pointer;}";
  html += ".curtain-box{display:flex;justify-content:space-between;align-items:flex-start;height:96px;padding:6px 8px 10px;background:#0c0517;border-radius:10px;border-top:3px solid #5a3e85;position:relative;}";
  html += ".strand{display:flex;flex-direction:column;justify-content:space-between;height:100%;align-items:center;width:4%;}";
  html += ".led{width:6px;height:6px;border-radius:50%;background:#2a1f3d;transition:all 0.12s ease;}";
  html += ".strand.on .led{background:#ffd166;box-shadow:0 0 8px #ff7b00, 0 0 14px #ffd166;}";
  html += ".ch-badges{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:8px;}";
  html += ".ch-pill{background:#241440;border:1px solid #4a3070;border-radius:6px;padding:4px;font-size:11px;font-weight:bold;color:#888;transition:0.12s;}";
  html += ".ch-pill.on{background:#ffb703;color:#120d2b;border-color:#fff;box-shadow:0 0 10px rgba(255,183,3,0.7);}";
  html += ".status{background:#1f1847;border:1px solid rgba(255,183,3,0.5);padding:9px;border-radius:10px;margin:0 auto 12px;max-width:420px;font-size:13px;line-height:1.5;}";
  html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:9px;max-width:420px;margin:0 auto;}";
  html += "a{text-decoration:none;}";
  html += "button{width:100%;padding:13px 8px;font-size:14px;font-weight:bold;border-radius:8px;border:none;cursor:pointer;color:#fff;background:#3a2d78;transition:0.15s;}";
  html += "button:active{transform:scale(0.97);}";
  html += ".active{background:#fb8500 !important;color:#000 !important;box-shadow:0 0 12px #fb8500;}";
  html += ".full{grid-column:span 2;}";
  html += ".guide-btn{background:linear-gradient(90deg,#ffd166,#ff7b00);color:#14051e;font-size:14px;}";
  html += ".auto{background:#8338ec;}";
  html += ".on{background:#2a9d8f;}";
  html += ".off{background:#e63946;}";
  html += ".speed-btn{background:#264653;font-size:13px;padding:10px;}";
  html += ".settings-btn{background:#0077b6;font-size:14px;padding:11px;margin-top:4px;}";
  html += ".ota-btn{background:#4a4e69;font-size:12px;padding:8px;margin-top:2px;}";
  html += "</style></head><body>";

  html += "<h1>&#129684; Diwali Light Controller &#10024;</h1>";

  // Live 20-String Vertical Curtain Visualizer Card
  html += "<div class='vis-card'>";
  html += "<div class='vis-header'><span>&#10024; <b>Live 20-String Balcony Preview</b></span>";
  html += "<button class='layout-toggle' id='layoutBtn' onclick='toggleLayout()'>Wiring: Interleaved (1-2-3-4)</button></div>";
  html += "<div class='curtain-box' id='curtainBox'></div>";
  html += "<div class='ch-badges'>";
  html += "<div class='ch-pill' id='ch0'>RELAY 1</div>";
  html += "<div class='ch-pill' id='ch1'>RELAY 2</div>";
  html += "<div class='ch-pill' id='ch2'>RELAY 3</div>";
  html += "<div class='ch-pill' id='ch3'>RELAY 4</div>";
  html += "</div></div>";

  // Dynamic Status Box
  html += "<div class='status'><b>Selected Mode:</b> <span id='modeTitle'>" + getModeName(currentMode) + "</span>";
  html += "<span id='subPattern'>" + String(currentMode == 10 ? "<br><small style='color:#ffd166;'>(Now Playing: " + getModeName(activePattern) + ")</small>" : "") + "</span>";
  html += "<br><b>Speed:</b> <span id='spdLabel'>" + String(stepDelay) + "</span> ms";
  if (isAPMode) {
    html += "<br><span style='color:#ff7b00;'>&#9888; Rescue AP Mode Active (192.168.4.1)</span>";
  }
  html += "</div>";

  // Instant Pattern & Speed Buttons
  html += "<div class='grid'>";
  html += "<a class='full' href='/guide'><button class='guide-btn'>&#128214; View Setup &amp; Wiring Guide</button></a>";
  html += "<button id='btn-m10' onclick='sendCmd(\"m=10\")' class='full auto " + String(currentMode==10?"active":"") + "'>&#128257; Auto-Cycle All 8 Patterns (30s)</button>";
  html += "<button id='btn-m9' onclick='sendCmd(\"m=9\")' class='on " + String(currentMode==9?"active":"") + "'>&#128161; All Steady ON</button>";
  html += "<button id='btn-m0' onclick='sendCmd(\"m=0\")' class='off " + String(currentMode==0?"active":"") + "'>&#128721; All OFF</button>";

  html += "<button id='btn-m1' onclick='sendCmd(\"m=1\")' class='" + String(currentMode==1?"active":"") + "'>1. Running Wave</button>";
  html += "<button id='btn-m2' onclick='sendCmd(\"m=2\")' class='" + String(currentMode==2?"active":"") + "'>2. Dark Wave</button>";
  html += "<button id='btn-m3' onclick='sendCmd(\"m=3\")' class='" + String(currentMode==3?"active":"") + "'>3. Ping-Pong</button>";
  html += "<button id='btn-m4' onclick='sendCmd(\"m=4\")' class='" + String(currentMode==4?"active":"") + "'>4. Theatre Curtain</button>";
  html += "<button id='btn-m5' onclick='sendCmd(\"m=5\")' class='" + String(currentMode==5?"active":"") + "'>5. Thick Ribbon</button>";
  html += "<button id='btn-m6' onclick='sendCmd(\"m=6\")' class='" + String(currentMode==6?"active":"") + "'>6. Fill &amp; Wipe</button>";
  html += "<button id='btn-m7' onclick='sendCmd(\"m=7\")' class='" + String(currentMode==7?"active":"") + "'>7. Odd / Even</button>";
  html += "<button id='btn-m8' onclick='sendCmd(\"m=8\")' class='" + String(currentMode==8?"active":"") + "'>8. Random Sparkle</button>";

  html += "<button id='btn-s400' onclick='sendCmd(\"spd=400\")' class='speed-btn " + String(stepDelay==400?"active":"") + "'>Fast (400ms)</button>";
  html += "<button id='btn-s650' onclick='sendCmd(\"spd=650\")' class='speed-btn " + String(stepDelay==650?"active":"") + "'>Medium (650ms)</button>";
  html += "<button id='btn-s1000' onclick='sendCmd(\"spd=1000\")' class='full speed-btn " + String(stepDelay==1000?"active":"") + "'>Slow &amp; Relaxed (1000ms)</button>";

  html += "<a class='full' href='/settings'><button class='settings-btn'>&#128272; Wi-Fi &amp; Security Settings</button></a>";
  html += "<a class='full' href='/update'><button class='ota-btn'>&#9881; Wireless Firmware Update (/update)</button></a>";
  html += "</div>";

  // Copyright & GitHub Footer
  html += FOOTER_SNIPPET;

  // JavaScript Pattern Simulator + Live ESP8266 Sync
  html += R"=====(
<script>
let currentMode = )=====" + String(currentMode) + R"=====(;
let activePattern = )=====" + String(activePattern) + R"=====(;
let stepDelay = )=====" + String(stepDelay) + R"=====(;
let interleaved = true;
let simStep = 0;
let simTimer = null;

const patterns = {
  1: [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
  2: [[0,1,1,1],[1,0,1,1],[1,1,0,1],[1,1,1,0]],
  3: [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1],[0,0,1,0],[0,1,0,0]],
  4: [[0,0,0,0],[0,1,1,0],[1,1,1,1],[1,0,0,1]],
  5: [[1,1,0,0],[0,1,1,0],[0,0,1,1],[1,0,0,1]],
  6: [[1,0,0,0],[1,1,0,0],[1,1,1,0],[1,1,1,1],[0,1,1,1],[0,0,1,1],[0,0,0,1],[0,0,0,0]],
  7: [[1,0,1,0],[0,1,0,1]]
};

const modeNames = {
  0: "All OFF",
  1: "1. Single Running Wave",
  2: "2. Inverted Dark Wave",
  3: "3. Knight Rider Ping-Pong",
  4: "4. Theatre Curtain (Center-Out)",
  5: "5. Rolling Thick Ribbon",
  6: "6. Curtain Fill & Wipe",
  7: "7. Odd / Even Dance",
  8: "8. Random Sparkle",
  9: "All Lights Steady ON",
  10: "Auto-Cycle All 8 Patterns (30s each)"
};

const box = document.getElementById('curtainBox');
for (let s = 0; s < 20; s++) {
  const strand = document.createElement('div');
  strand.className = 'strand';
  strand.id = 's' + s;
  for (let l = 0; l < 6; l++) {
    const led = document.createElement('div');
    led.className = 'led';
    strand.appendChild(led);
  }
  box.appendChild(strand);
}

function toggleLayout() {
  interleaved = !interleaved;
  document.getElementById('layoutBtn').innerText = interleaved
    ? 'Wiring: Interleaved (1-2-3-4)'
    : 'Wiring: 4 Blocks (5 per block)';
}

function renderRelays(r) {
  for (let c = 0; c < 4; c++) {
    const pill = document.getElementById('ch' + c);
    if (r[c]) pill.classList.add('on');
    else pill.classList.remove('on');
  }
  for (let s = 0; s < 20; s++) {
    const ch = interleaved ? (s % 4) : Math.floor(s / 5);
    const strand = document.getElementById('s' + s);
    if (r[ch]) strand.classList.add('on');
    else strand.classList.remove('on');
  }
}

function stepVisualizer() {
  if (currentMode === 0) { renderRelays([0,0,0,0]); return; }
  if (currentMode === 9) { renderRelays([1,1,1,1]); return; }
  const pat = (currentMode === 10) ? activePattern : currentMode;
  if (pat === 8) {
    renderRelays([Math.round(Math.random()), Math.round(Math.random()), Math.round(Math.random()), 1]);
    return;
  }
  const frames = patterns[pat] || patterns[1];
  simStep = simStep % frames.length;
  renderRelays(frames[simStep]);
  simStep = (simStep + 1) % frames.length;
}

function restartSimTimer() {
  if (simTimer) clearInterval(simTimer);
  stepVisualizer();
  simTimer = setInterval(stepVisualizer, stepDelay);
}

function updateUIButtons() {
  document.getElementById('modeTitle').innerText = modeNames[currentMode] || 'Unknown';
  document.getElementById('subPattern').innerHTML = (currentMode === 10)
    ? "<br><small style='color:#ffd166;'>(Now Playing: " + (modeNames[activePattern] || '') + ")</small>"
    : "";
  document.getElementById('spdLabel').innerText = stepDelay;

  for (let m = 0; m <= 10; m++) {
    const b = document.getElementById('btn-m' + m);
    if (b) {
      if (m === currentMode) b.classList.add('active');
      else b.classList.remove('active');
    }
  }
  [400, 650, 1000].forEach(s => {
    const sb = document.getElementById('btn-s' + s);
    if (sb) {
      if (s === stepDelay) sb.classList.add('active');
      else sb.classList.remove('active');
    }
  });
}

function sendCmd(query) {
  fetch('/set?' + query + '&ajax=1')
    .then(r => r.json())
    .then(d => {
      const spdChanged = (d.spd !== stepDelay);
      currentMode = d.m;
      activePattern = d.p;
      stepDelay = d.spd;
      simStep = d.step;
      updateUIButtons();
      if (spdChanged) restartSimTimer();
      else renderRelays(d.r);
    }).catch(e => {});
}

setInterval(() => {
  fetch('/status')
    .then(r => r.json())
    .then(d => {
      const spdChanged = (d.spd !== stepDelay);
      currentMode = d.m;
      activePattern = d.p;
      stepDelay = d.spd;
      updateUIButtons();
      if (spdChanged) restartSimTimer();
    }).catch(e => {});
}, 2000);

restartSimTimer();
</script>
)=====";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleStatus() {
  if (!checkAuth()) return;
  String json = "{\"m\":" + String(currentMode) +
                ",\"p\":" + String(activePattern) +
                ",\"spd\":" + String(stepDelay) +
                ",\"step\":" + String(stepIndex) +
                ",\"r\":[" + String(currentRelayState[0]) + "," +
                             String(currentRelayState[1]) + "," +
                             String(currentRelayState[2]) + "," +
                             String(currentRelayState[3]) + "]}";
  server.send(200, "application/json", json);
}

// --- 9. WIFI & ADMIN SETTINGS PAGE ---
void handleSettings() {
  if (!checkAuth()) return;

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Wi-Fi & Security Settings</title>";
  html += "<style>";
  html += "body{font-family:system-ui,sans-serif;background:#120d2b;color:#fff;margin:0;padding:15px;}";
  html += ".card{background:#1f1847;border:1px solid #ffb703;border-radius:12px;padding:20px;max-width:420px;margin:0 auto;}";
  html += "h2{color:#ffb703;margin-top:0;text-align:center;}";
  html += "label{display:block;margin:12px 0 4px;font-size:14px;color:#ffd166;font-weight:bold;}";
  html += "input{width:100%;padding:10px;border-radius:6px;border:1px solid #555;background:#120d2b;color:#fff;font-size:15px;box-sizing:border-box;}";
  html += "button{width:100%;padding:14px;margin-top:18px;font-size:16px;font-weight:bold;border-radius:8px;border:none;cursor:pointer;background:#fb8500;color:#000;}";
  html += ".back{display:block;text-align:center;margin-top:15px;color:#06d6a0;text-decoration:none;font-weight:bold;}";
  html += "hr{border:0;border-top:1px solid rgba(255,255,255,0.15);margin:18px 0;}";
  html += "</style></head><body>";

  html += "<div class='card'><h2>&#128272; Wi-Fi &amp; Login Settings</h2>";
  html += "<form method='POST' action='/save_settings'>";
  html += "<label>Wi-Fi Network Name (SSID):</label>";
  html += "<input type='text' name='ssid' maxlength='32' value='" + String(config.wifi_ssid) + "' required>";
  html += "<label>Wi-Fi Password (leave blank to keep current):</label>";
  html += "<input type='password' name='wpass' maxlength='64' placeholder='Unchanged if left blank'>";

  html += "<hr>";
  html += "<label>Dashboard Admin Username:</label>";
  html += "<input type='text' name='user' maxlength='32' value='" + String(config.www_user) + "' required>";
  html += "<label>Dashboard &amp; OTA Password (leave blank to keep current):</label>";
  html += "<input type='password' name='apass' maxlength='32' placeholder='Unchanged if left blank'>";

  html += "<hr>";
  html += "<label style='color:#ff7b00;'>Confirm Current Admin Password (Required):</label>";
  html += "<input type='password' name='curpass' maxlength='32' required placeholder='Enter current password to save'>";

  html += "<button type='submit'>&#128190; Save &amp; Reboot Board</button>";
  html += "</form>";
  html += "<a class='back' href='/'>&larr; Back to Control Dashboard</a></div>";
  html += FOOTER_SNIPPET;
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSaveSettings() {
  if (!checkAuth()) return;

  if (!server.hasArg("curpass") || server.arg("curpass") != String(config.www_pass)) {
    server.send(403, "text/html", "<body style='background:#120d2b;color:#fff;text-align:center;padding:40px;font-family:sans-serif;'><h2 style='color:#e63946;'>Current Password Incorrect!</h2><a href='/settings' style='color:#ffd166;'>Go Back</a>" + String(FOOTER_SNIPPET) + "</body>");
    return;
  }

  String newSsid = server.arg("ssid");
  String newWpass = server.arg("wpass");
  String newUser = server.arg("user");
  String newApass = server.arg("apass");

  newSsid.trim();
  newUser.trim();

  if (newSsid.length() > 0 && newSsid.length() < 33) {
    strncpy(config.wifi_ssid, newSsid.c_str(), sizeof(config.wifi_ssid) - 1);
    config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
  }
  if (newWpass.length() > 0 && newWpass.length() < 65) {
    strncpy(config.wifi_pass, newWpass.c_str(), sizeof(config.wifi_pass) - 1);
    config.wifi_pass[sizeof(config.wifi_pass) - 1] = '\0';
  }
  if (newUser.length() > 0 && newUser.length() < 33) {
    strncpy(config.www_user, newUser.c_str(), sizeof(config.www_user) - 1);
    config.www_user[sizeof(config.www_user) - 1] = '\0';
  }
  if (newApass.length() > 0 && newApass.length() < 33) {
    strncpy(config.www_pass, newApass.c_str(), sizeof(config.www_pass) - 1);
    config.www_pass[sizeof(config.www_pass) - 1] = '\0';
  }

  saveConfig();

  String msg = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='8;url=/'><meta name='viewport' content='width=device-width,initial-scale=1'></head>";
  msg += "<body style='background:#120d2b;color:#fff;text-align:center;padding:40px;font-family:sans-serif;'>";
  msg += "<h2 style='color:#06d6a0;'>&#9989; Settings Saved to EEPROM!</h2>";
  msg += "<p>Rebooting ESP8266 and connecting to <b>" + String(config.wifi_ssid) + "</b>...</p>";
  msg += "<p><small>If Wi-Fi fails to connect within 20 seconds, connect to hotspot <b>Diwali-Setup-AP</b> (pass: diwali2026) and open http://192.168.4.1</small></p>";
  msg += FOOTER_SNIPPET;
  msg += "</body></html>";
  server.send(200, "text/html", msg);
  delay(1000);
  ESP.restart();
}

void runPattern(int pat) {
  switch (pat) {
    case 1:
      setRelays(wave[stepIndex][0], wave[stepIndex][1], wave[stepIndex][2], wave[stepIndex][3]);
      stepIndex = (stepIndex + 1) % 4;
      break;
    case 2:
      setRelays(darkWave[stepIndex][0], darkWave[stepIndex][1], darkWave[stepIndex][2], darkWave[stepIndex][3]);
      stepIndex = (stepIndex + 1) % 4;
      break;
    case 3:
      setRelays(pingPong[stepIndex][0], pingPong[stepIndex][1], pingPong[stepIndex][2], pingPong[stepIndex][3]);
      stepIndex = (stepIndex + 1) % 6;
      break;
    case 4:
      setRelays(curtain[stepIndex][0], curtain[stepIndex][1], curtain[stepIndex][2], curtain[stepIndex][3]);
      stepIndex = (stepIndex + 1) % 4;
      break;
    case 5:
      setRelays(ribbon[stepIndex][0], ribbon[stepIndex][1], ribbon[stepIndex][2], ribbon[stepIndex][3]);
      stepIndex = (stepIndex + 1) % 4;
      break;
    case 6:
      setRelays(fillWipe[stepIndex][0], fillWipe[stepIndex][1], fillWipe[stepIndex][2], fillWipe[stepIndex][3]);
      stepIndex = (stepIndex + 1) % 8;
      break;
    case 7:
      setRelays(oddEven[stepIndex][0], oddEven[stepIndex][1], oddEven[stepIndex][2], oddEven[stepIndex][3]);
      stepIndex = (stepIndex + 1) % 2;
      break;
    case 8:
      setRelays(random(0, 2), random(0, 2), random(0, 2), 1);
      break;
  }
}

void handleSet() {
  if (!checkAuth()) return;

  if (server.hasArg("m")) {
    currentMode = server.arg("m").toInt();
    stepIndex = 0;
    if (currentMode == 0) {
      setRelays(0, 0, 0, 0);
    } else if (currentMode == 9) {
      setRelays(1, 1, 1, 1);
    } else {
      if (currentMode == 10) {
        activePattern = 1;
        lastAutoSwitch = millis();
      }
      runPattern(currentMode == 10 ? activePattern : currentMode);
      lastStepTime = millis();
    }
  }
  if (server.hasArg("spd")) {
    long newSpd = server.arg("spd").toInt();
    if (newSpd >= 400 && newSpd <= 3000) stepDelay = newSpd;
  }

  if (server.hasArg("ajax")) {
    handleStatus();
    return;
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

// --- 10. SETUP & MAIN LOOP ---
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) {
    pinMode(relays[i], OUTPUT);
    digitalWrite(relays[i], RELAY_OFF);
  }
  pinMode(flashBtnPin, INPUT_PULLUP);
  randomSeed(analogRead(A0));

  loadConfig();

  WiFi.mode(WIFI_STA);
  WiFi.hostname("diwali");
  WiFi.begin(config.wifi_ssid, config.wifi_pass);
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(config.wifi_ssid);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    isAPMode = false;
    Serial.print("\nConnected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    isAPMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Diwali-Setup-AP", "diwali2026");
    Serial.println("\nWi-Fi failed! Started Rescue Hotspot: Diwali-Setup-AP (IP: 192.168.4.1)");
  }

  if (MDNS.begin("diwali")) {
    MDNS.addService("http", "tcp", 80);
  }

  httpUpdater.setup(&server, "/update", config.www_user, config.www_pass);

  ArduinoOTA.setHostname("diwali");
  ArduinoOTA.setPassword(config.www_pass);
  ArduinoOTA.onStart([]() {
    setRelays(0, 0, 0, 0);
  });
  ArduinoOTA.begin();

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/status", handleStatus);
  server.on("/settings", handleSettings);
  server.on("/save_settings", HTTP_POST, handleSaveSettings);
  server.on("/guide", handleGuide);
  server.begin();
}

unsigned long flashHoldStart = 0;

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();
  unsigned long now = millis();

  if (digitalRead(flashBtnPin) == LOW) {
    if (flashHoldStart == 0) flashHoldStart = now;
    else if (now - flashHoldStart >= 5000) {
      resetToDefaults();
      ESP.restart();
    }
  } else {
    flashHoldStart = 0;
  }

  if (currentMode == 0) {
    setRelays(0, 0, 0, 0);
    return;
  }
  if (currentMode == 9) {
    setRelays(1, 1, 1, 1);
    return;
  }

  int patternToPlay = currentMode;
  if (currentMode == 10) {
    if (now - lastAutoSwitch >= 30000) {
      lastAutoSwitch = now;
      activePattern = (activePattern % 8) + 1;
      stepIndex = 0;
    }
    patternToPlay = activePattern;
  }

  if (now - lastStepTime >= stepDelay) {
    lastStepTime = now;
    runPattern(patternToPlay);
  }
}
