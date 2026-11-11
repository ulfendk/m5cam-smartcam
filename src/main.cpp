// SmartCam firmware for the M5Camera (M5CAM / M5CAM F).
//
// Serves the OV2640 as an MJPEG stream that go2rtc (and so SmartCam) reads directly:
//   http://<cam>:81/stream   MJPEG (multipart/x-mixed-replace)
//   http://<cam>/capture     one JPEG
//   http://<cam>/status      JSON: settings, uptime, RSSI, frame rate
//   http://<cam>/control?var=<name>&val=<n>   change a sensor setting (saved across reboots)
//   POST http://<cam>/restart, POST http://<cam>/update (raw firmware.bin body: OTA)
//
// WiFi: set over USB serial (ssid=..., pass=..., then reboot) or through the captive portal
// "SmartCam-XXXXXX" that opens when no network can be joined. An optional password protects
// every endpoint with HTTP basic auth (user "admin").

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <mbedtls/base64.h>

#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

// M5Camera model B pin map (the M5CAM F "fisheye" board), read from the factory firmware's boot log.
static const camera_config_t kCameraPins = {
    .pin_pwdn = -1, .pin_reset = 15, .pin_xclk = 27, .pin_sscb_sda = 22, .pin_sscb_scl = 23,
    .pin_d7 = 19, .pin_d6 = 36, .pin_d5 = 18, .pin_d4 = 39, .pin_d3 = 5, .pin_d2 = 34,
    .pin_d1 = 35, .pin_d0 = 32, .pin_vsync = 25, .pin_href = 26, .pin_pclk = 21,
};

static Preferences prefs;
static httpd_handle_t ctrlServer = nullptr, streamServer = nullptr;
static String hostname, authHeader;  // authHeader: "Basic <base64>" or empty when no password is set
static volatile uint32_t framesSent = 0;
static uint32_t captureFailures = 0;
static float streamFps = 0;

// Sensor settings that are persisted. Applied in this order at boot.
struct Setting { const char *name; int def, lo, hi; };
static const Setting kSettings[] = {
    {"framesize", FRAMESIZE_XGA, FRAMESIZE_QVGA, FRAMESIZE_UXGA},  // 1024x768: ~8 fps on this board
    {"quality", 12, 4, 63},       // JPEG quality: lower is better and larger
    {"brightness", 0, -2, 2},  {"contrast", 0, -2, 2},  {"saturation", 0, -2, 2},
    {"vflip", 0, 0, 1},        {"hmirror", 0, 0, 1},
    {"awb", 1, 0, 1},          {"aec", 1, 0, 1},        {"agc", 1, 0, 1},
    {"gainceiling", 2, 0, 6},  {"lenc", 1, 0, 1},       {"dcw", 1, 0, 1},
    {"maxfps", 15, 1, 30},     // stream frame-rate cap (firmware-side)
};

static int getSetting(const char *name) {
  for (auto &s : kSettings)
    if (!strcmp(s.name, name)) return prefs.getInt(s.name, s.def);
  return 0;
}

static bool applySetting(const char *name, int v) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return false;
  if (!strcmp(name, "framesize")) return s->set_framesize(s, (framesize_t)v) == 0;
  if (!strcmp(name, "quality")) return s->set_quality(s, v) == 0;
  if (!strcmp(name, "brightness")) return s->set_brightness(s, v) == 0;
  if (!strcmp(name, "contrast")) return s->set_contrast(s, v) == 0;
  if (!strcmp(name, "saturation")) return s->set_saturation(s, v) == 0;
  if (!strcmp(name, "vflip")) return s->set_vflip(s, v) == 0;
  if (!strcmp(name, "hmirror")) return s->set_hmirror(s, v) == 0;
  if (!strcmp(name, "awb")) return s->set_whitebal(s, v) == 0;
  if (!strcmp(name, "aec")) return s->set_exposure_ctrl(s, v) == 0;
  if (!strcmp(name, "agc")) return s->set_gain_ctrl(s, v) == 0;
  if (!strcmp(name, "gainceiling")) return s->set_gainceiling(s, (gainceiling_t)v) == 0;
  if (!strcmp(name, "lenc")) return s->set_lenc(s, v) == 0;
  if (!strcmp(name, "dcw")) return s->set_dcw(s, v) == 0;
  if (!strcmp(name, "maxfps")) return true;
  return false;
}

static bool initCamera() {
  camera_config_t c = kCameraPins;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  // Allocate for the largest size so framesize can be raised at runtime without a reboot.
  c.frame_size = FRAMESIZE_UXGA;
  c.jpeg_quality = getSetting("quality");
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;  // always the newest frame: no latency build-up
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("camera init failed: 0x%x\n", err);
    return false;
  }
  for (auto &s : kSettings) applySetting(s.name, getSetting(s.name));
  return true;
}

// ---------- HTTP ----------

static bool authorized(httpd_req_t *req) {
  if (authHeader.isEmpty()) return true;
  char buf[160];
  if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof buf) == ESP_OK && authHeader == buf) return true;
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"SmartCam\"");
  httpd_resp_sendstr(req, "unauthorized");
  return false;
}

static esp_err_t captureHandler(httpd_req_t *req) {
  if (!authorized(req)) return ESP_OK;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    captureFailures++;
    return httpd_resp_send_500(req);
  }
  captureFailures = 0;
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t r = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return r;
}

#define BOUNDARY "smartcamframe"
static esp_err_t streamHandler(httpd_req_t *req) {
  if (!authorized(req)) return ESP_OK;
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char part[96];
  uint32_t last = 0, windowStart = millis(), windowFrames = 0;
  while (true) {
    uint32_t minGap = 1000 / max(1, getSetting("maxfps"));
    uint32_t now = millis();
    if (now - last < minGap) delay(minGap - (now - last));
    last = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      if (++captureFailures > 20) return ESP_FAIL;  // the main loop restarts the camera
      delay(50);
      continue;
    }
    captureFailures = 0;
    int n = snprintf(part, sizeof part, "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    esp_err_t r = httpd_resp_send_chunk(req, part, n);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (r != ESP_OK) return ESP_OK;  // client went away
    framesSent++;
    if (++windowFrames, millis() - windowStart >= 5000) {
      streamFps = windowFrames * 1000.0f / (millis() - windowStart);
      windowStart = millis();
      windowFrames = 0;
    }
  }
}

static String statusJson() {
  String j = "{";
  j += "\"firmware\":\"" FW_VERSION "\",\"hostname\":\"" + hostname + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"uptime_s\":" + String(millis() / 1000) + ",\"frames_sent\":" + String(framesSent) + ",";
  j += "\"stream_fps\":" + String(streamFps, 1) + ",\"free_psram\":" + String(ESP.getFreePsram()) + ",";
  j += "\"auth\":" + String(authHeader.isEmpty() ? "false" : "true");
  for (auto &s : kSettings) j += ",\"" + String(s.name) + "\":" + String(getSetting(s.name));
  return j + "}";
}

static esp_err_t statusHandler(httpd_req_t *req) {
  if (!authorized(req)) return ESP_OK;
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, statusJson().c_str());
}

static esp_err_t controlHandler(httpd_req_t *req) {
  if (!authorized(req)) return ESP_OK;
  char q[96], var[24], val[12];
  if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK ||
      httpd_query_key_value(q, "var", var, sizeof var) != ESP_OK ||
      httpd_query_key_value(q, "val", val, sizeof val) != ESP_OK)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need var and val");
  int v = atoi(val);
  for (auto &s : kSettings) {
    if (strcmp(s.name, var)) continue;
    if (v < s.lo || v > s.hi) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value out of range");
    if (!applySetting(var, v)) return httpd_resp_send_500(req);
    prefs.putInt(s.name, v);
    return statusHandler(req);
  }
  return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown var");
}

static esp_err_t restartHandler(httpd_req_t *req) {
  if (!authorized(req)) return ESP_OK;
  httpd_resp_sendstr(req, "restarting\n");
  delay(300);
  ESP.restart();
  return ESP_OK;
}

static esp_err_t updateHandler(httpd_req_t *req) {
  if (!authorized(req)) return ESP_OK;
  if (!Update.begin(req->content_len)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, Update.errorString());
  char buf[2048];
  size_t left = req->content_len;
  while (left > 0) {
    int n = httpd_req_recv(req, buf, min(left, sizeof buf));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {
      Update.abort();
      return ESP_FAIL;
    }
    if (Update.write((uint8_t *)buf, n) != (size_t)n) {
      Update.abort();
      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, Update.errorString());
    }
    left -= n;
  }
  if (!Update.end(true)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, Update.errorString());
  httpd_resp_sendstr(req, "update ok, restarting\n");
  delay(500);
  ESP.restart();
  return ESP_OK;
}

static const char kIndex[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SmartCam M5CAM</title>
<style>body{font:15px system-ui;margin:16px;background:#111;color:#eee}img{max-width:100%;background:#000}
label{display:inline-block;margin:4px 12px 4px 0}select,input{background:#222;color:#eee;border:1px solid #444}
code{background:#222;padding:2px 4px}</style></head><body>
<h2>SmartCam M5CAM</h2><img id="v"><p>Stream: <code id="u"></code></p><div id="c"></div><pre id="s"></pre>
<script>
const sizes={5:'320x240',6:'400x296',7:'480x320',8:'640x480',9:'800x600 (~14 fps)',10:'1024x768 (~8 fps)',11:'1280x720 (~8 fps)',12:'1280x1024',13:'1600x1200 (~5 fps)'};
const u=`http://${location.hostname}:81/stream`;v.src=u;document.getElementById('u').textContent=u;
async function load(){const s=await (await fetch('/status')).json();document.getElementById('s').textContent=JSON.stringify(s,null,1);return s}
function ctl(k,val){fetch(`/control?var=${k}&val=${val}`).then(load)}
load().then(s=>{let h='<label>framesize <select onchange="ctl(\'framesize\',this.value)">';
for(const[k,n] of Object.entries(sizes))h+=`<option value=${k} ${s.framesize==k?'selected':''}>${n}</option>`;h+='</select></label>';
for(const k of['quality','maxfps','brightness','contrast','saturation'])h+=`<label>${k} <input type=number value=${s[k]} style="width:4em" onchange="ctl('${k}',this.value)"></label>`;
for(const k of['vflip','hmirror','awb','aec','agc','lenc'])h+=`<label><input type=checkbox ${s[k]?'checked':''} onchange="ctl('${k}',this.checked?1:0)"> ${k}</label>`;
document.getElementById('c').innerHTML=h});
</script></body></html>)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  if (!authorized(req)) return ESP_OK;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kIndex, HTTPD_RESP_USE_STRLEN);
}

static void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.max_uri_handlers = 8;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 10;
  httpd_uri_t uris[] = {
      {"/", HTTP_GET, indexHandler, nullptr},          {"/capture", HTTP_GET, captureHandler, nullptr},
      {"/status", HTTP_GET, statusHandler, nullptr},   {"/control", HTTP_GET, controlHandler, nullptr},
      {"/restart", HTTP_POST, restartHandler, nullptr}, {"/update", HTTP_POST, updateHandler, nullptr},
  };
  if (httpd_start(&ctrlServer, &cfg) == ESP_OK)
    for (auto &u : uris) httpd_register_uri_handler(ctrlServer, &u);

  // The stream gets its own server, so a client watching it never blocks /capture or /status.
  cfg.server_port = 81;
  cfg.ctrl_port = 32769;
  cfg.max_open_sockets = 3;
  cfg.lru_purge_enable = true;
  httpd_uri_t s = {"/stream", HTTP_GET, streamHandler, nullptr};
  if (httpd_start(&streamServer, &cfg) == ESP_OK) httpd_register_uri_handler(streamServer, &s);
}

// ---------- WiFi, auth, serial provisioning ----------

static void loadAuth() {
  String pw = prefs.getString("admin", "");
  authHeader = "";
  if (pw.isEmpty()) return;
  String plain = "admin:" + pw;
  unsigned char out[128];
  size_t n = 0;
  mbedtls_base64_encode(out, sizeof out, &n, (const unsigned char *)plain.c_str(), plain.length());
  authHeader = "Basic " + String((const char *)out).substring(0, n);
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') {
      if (line.length() < 200) line += ch;
      continue;
    }
    String cmd = line;
    line = "";
    cmd.trim();
    int eq = cmd.indexOf('=');
    String key = eq < 0 ? cmd : cmd.substring(0, eq), val = eq < 0 ? "" : cmd.substring(eq + 1);
    if (key == "ssid") { prefs.putString("ssid", val); Serial.println("ok ssid"); }
    else if (key == "pass") { prefs.putString("pass", val); Serial.println("ok pass"); }
    else if (key == "admin") { prefs.putString("admin", val); loadAuth(); Serial.println("ok admin"); }
    else if (key == "status") Serial.println(statusJson());
    else if (key == "reboot") { Serial.println("ok reboot"); delay(100); ESP.restart(); }
    else if (key == "factory") { prefs.clear(); Serial.println("ok factory"); delay(100); ESP.restart(); }
    else if (key.length()) Serial.println("commands: ssid=<name> pass=<psk> admin=<password|empty> status reboot factory");
  }
}

static bool connectWiFi(uint32_t timeoutMs) {
  String ssid = prefs.getString("ssid", "");
  if (ssid.isEmpty()) return false;
  WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    handleSerial();
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// No known network: open the "SmartCam-XXXXXX" portal for 5 minutes, then try again from scratch.
static void runPortal() {
  WiFiManager wm;
  WiFiManagerParameter adminParam("admin", "Camera password (optional, user 'admin')", "", 40);
  wm.addParameter(&adminParam);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(300);
  String ap = "SmartCam-" + hostname.substring(hostname.length() - 6);
  Serial.printf("no WiFi: portal on AP %s (or set ssid=/pass= here)\n", ap.c_str());
  wm.startConfigPortal(ap.c_str());
  uint32_t start = millis();
  while (millis() - start < 300000) {
    if (wm.process()) {
      prefs.putString("ssid", wm.getWiFiSSID());
      prefs.putString("pass", wm.getWiFiPass());
      if (strlen(adminParam.getValue())) prefs.putString("admin", adminParam.getValue());
      break;
    }
    handleSerial();
    delay(10);
  }
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  prefs.begin("smartcam", false);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char host[24];
  snprintf(host, sizeof host, "smartcam-%02x%02x%02x", mac[3], mac[4], mac[5]);
  hostname = host;
  Serial.printf("\nSmartCam M5CAM firmware %s, %s\n", FW_VERSION, host);
  loadAuth();

  if (!initCamera()) {
    delay(2000);
    ESP.restart();
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host);
  WiFi.setSleep(false);  // modem sleep adds hundreds of ms of jitter to the stream
  WiFi.setAutoReconnect(true);
  if (!connectWiFi(30000)) runPortal();

  MDNS.begin(host);
  MDNS.addService("http", "tcp", 80);
  startServers();
  Serial.printf("ready: http://%s/  stream: http://%s:81/stream\n", WiFi.localIP().toString().c_str(),
                WiFi.localIP().toString().c_str());
}

void loop() {
  static uint32_t offlineSince = 0;
  handleSerial();
  // Self-healing: SmartCam's watchdog can power-cycle the camera, but most stalls never need it.
  if (WiFi.status() != WL_CONNECTED) {
    if (!offlineSince) offlineSince = millis();
    if (millis() - offlineSince > 60000) ESP.restart();
  } else {
    offlineSince = 0;
  }
  if (captureFailures > 20) {
    Serial.println("camera stalled, restarting");
    ESP.restart();
  }
  delay(100);
}
