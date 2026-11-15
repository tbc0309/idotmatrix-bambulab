#include "IDotMatrixHardwareConfig.h"

#if defined(IDOTMATRIX_BOARD_CLOCKWISE)
#include "ClockwiseWifiClock.h"
#include "ClockwiseTimeZone.h"
#include "ClockwiseTimeZoneOptions.h"
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <cerrno>
#include <sys/socket.h>

namespace clockwiseWifi {
namespace {
constexpr char settingsNamespace[] = "cw-wifi";
constexpr char settingsKey[] = "profile";
constexpr char defaultZone[] = "China Standard Time";
constexpr uint32_t setupWindow = 10 * 60 * 1000;
constexpr uint32_t connectTimeout = 45000;
constexpr uint32_t retryDelay = 5 * 60 * 1000;
constexpr time_t earliestTime = 1704067200;
enum class Stage { Start, Setup, Joining, ChooseZone, Complete, Online, Unconfigured };

struct Profile {
  char ssid[33];
  char password[65];
  char zone[64];
};

Profile profile{};
DNSServer dns;
WebServer http(80);
Stage stage = Stage::Start;
bool httpActive = false;
bool portalActive = false;
bool initialSetup = false;
bool sntpActive = false;
bool clockReady = false;
bool stationFailed = false;
bool saveFailed = false;
bool scanPending = false;
uint32_t scanStarted = 0;
bool (*read24Hour)() = nullptr;
void (*write24Hour)(bool) = nullptr;

bool english() { return http.arg("lang") == "en"; }
const char *label(const char *zh, const char *en) { return english() ? en : zh; }

uint32_t deadline = 0;
char formToken[17];
char apName[33];

constexpr char pageStart[] = R"html(<head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Clockwise 64×64 · iDotMatrix App</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#fff5eb;color:#35251b;font:15px/1.5 system-ui,sans-serif}
main{max-width:480px;margin:auto;padding:clamp(16px,5vw,40px) 16px}
main:has(.settings){max-width:600px}
header{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;border-bottom:2px solid #ec9a5b;padding-bottom:14px;margin-bottom:16px}
h1,h2,p{margin:0}
h1{font-size:clamp(13px,3.2vw,16px);line-height:1.4}
h1 span{color:#a84412}
h2{font-size:17px;margin-bottom:12px}
.card{background:#fffdfb;border:1px solid #ead0bc;border-radius:14px;padding:16px;min-width:0}
.setup{max-width:480px;margin:auto}
.settings{display:grid;gap:12px}
.status{background:#ffead6;border-radius:10px;padding:10px 14px;margin-bottom:12px;display:flex;flex-wrap:wrap;gap:4px 18px}
.status a{color:#a84412}.status time{font-variant-numeric:tabular-nums}
label{display:block;font-size:14px;font-weight:600}
input:not([type=hidden],[type=checkbox]),select,button{width:100%;font:inherit;min-height:42px;border-radius:8px;padding:7px 10px}
input:not([type=hidden],[type=checkbox]),select{border:1px solid #cda88f;background:#fff;color:inherit;margin:4px 0 10px;min-width:0}
nav select{margin:0;width:auto;font-size:13px}
button{background:#b84914;color:#fff;border:1px solid #b84914;font-weight:650}
button:hover{background:#9c3b10}
button:disabled{opacity:.55}
:focus-visible{outline:2px solid #a84412;outline-offset:2px}
.scan-row{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;align-items:end}
.scan-row button{width:auto;margin-bottom:10px;background:#ffead6;color:#8b350e;border-color:#dfb28e;white-space:nowrap}
#scanStatus:not(:empty),.hint{font-size:13px;color:#75584a;margin:0 0 10px}
[role=alert]{color:#913610;margin-bottom:12px}
.card-head{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:12px}
.card-head h2,.card-head .check{margin:0}.card-head .check{min-height:24px;white-space:nowrap}
.check{display:flex;align-items:center;gap:8px;min-height:42px;margin:0 0 10px}.check input{accent-color:#b84914;width:18px;height:18px;margin:0}
</style></head><body><main>)html";

bool expired(uint32_t now) { return static_cast<int32_t>(now - deadline) >= 0; }

bool saveProfile() {
  Preferences storage;
  return storage.begin(settingsNamespace) &&
         storage.putBytes(settingsKey, &profile, sizeof(profile)) == sizeof(profile);
}

bool knownZone(const char *name) {
  char rule[64];
  return bridge::timezone::resolve(name, earliestTime, rule, sizeof(rule));
}

bool stationConnected() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(); }

bool portalClient() { return portalActive && http.client().localIP() == WiFi.softAPIP(); }

bool allowedClient() {
  if (portalClient()) return true;
  const IPAddress local = WiFi.localIP();
  const String ip = local.toString();
  const String host = http.hostHeader();
  const bool expectedHost = host == ip || host == ip + ":80";
  const IPAddress mask = WiFi.subnetMask();
  const IPAddress peer = http.client().remoteIP();
  if (expectedHost && stationConnected() && http.client().localIP() == local &&
      (uint32_t(local) & uint32_t(mask)) == (uint32_t(peer) & uint32_t(mask))) return true;
  http.send(403, "text/plain", "Open the setup hotspot or use the device IP on your local network.");
  return false;
}

struct PageOutput {
  NetworkClient client = http.client();
  const uint32_t started = millis();
  bool writable = true;

  void write(const char *data, size_t size) {
    while (writable && size) {
      if (millis() - started > 4500) {
        Serial.printf("Wi-Fi HTTP: transmit timeout (heap=%lu)\n", static_cast<unsigned long>(ESP.getFreeHeap()));
        writable = false;
        break;
      }
      const size_t chunkSize = size < 768 ? size : 768;
      const int count = send(client.fd(), data, chunkSize, MSG_DONTWAIT);
      if (count > 0) {
        data += count;
        size -= count;
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOMEM || errno == ENOBUFS)) {
        delay(2);
      } else {
        Serial.printf("Wi-Fi HTTP: transmit failed (errno=%d)\n", errno);
        writable = false;
      }
    }
    if (!writable) client.stop();
  }
};

struct HtmlPage {
  PageOutput output;
  char buffer[1024];
  size_t buffered = 0;

  void flush() {
    output.write(buffer, buffered);
    buffered = 0;
  }

  void append(const char *data, size_t length) {
    while (output.writable && length) {
      const size_t available = sizeof(buffer) - buffered;
      const size_t count = length < available ? length : available;
      memcpy(buffer + buffered, data, count);
      buffered += count;
      data += count;
      length -= count;
      if (buffered == sizeof(buffer)) flush();
    }
  }

  void part(const char *data) {
    const size_t size = strlen(data);
    if (!size) return;
    char chunk[12];
    const int length = snprintf(chunk, sizeof(chunk), "%x\r\n", static_cast<unsigned>(size));
    append(chunk, length);
    append(data, size);
    append("\r\n", 2);
  }

  void part(const String &data) { part(data.c_str()); }

  void open() {
    constexpr char header[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                              "Cache-Control: no-store\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
    append(header, sizeof(header) - 1);
    part(label("<!doctype html><html lang=zh-CN>", "<!doctype html><html lang=en>"));
    part(pageStart);
    part("<header><h1>Clockwise 64×64 · <span>iDotMatrix App</span></h1>"
         "<nav><select aria-label=\"Language / 语言\" id=lang onchange=\"location.href='/?lang='+this.value\">");
    part(label("<option value=zh selected>简体中文</option><option value=en>English</option>",
               "<option value=zh>简体中文</option><option value=en selected>English</option>"));
    part("</select></nav></header>");
  }

  void close() {
    part("</main></body></html>");
    append("0\r\n\r\n", 5);
    if (buffered) flush();
  }
};

void beginForm(HtmlPage &page, const char *path) {
  char form[160];
  snprintf(form, sizeof(form), "<form method=post action=%s><input type=hidden name=token value=%s><input type=hidden name=lang value=%s>",
           path, formToken, label("zh", "en"));
  page.part(form);
}

void showNetworkForm(HtmlPage &page, bool autoScan) {
  beginForm(page, "/wifi");
  page.part(label("<div class=scan-row><label>附近 Wi-Fi", "<div class=scan-row><label>Nearby Wi-Fi"));
  page.part("<select id=net onchange=\"if(this.value)document.getElementById('ssid').value=this.value\"></select></label>");
  page.part(label("<button type=button id=scan>扫描 Wi-Fi</button></div><p id=scanStatus role=status></p><label>Wi-Fi 名称（SSID）",
                  "<button type=button id=scan>Scan Wi-Fi</button></div><p id=scanStatus role=status></p><label>Wi-Fi name (SSID)"));
  page.part("<input id=ssid name=ssid maxlength=32 required autocomplete=off></label>");
  page.part(label("<label>Wi-Fi 密码", "<label>Wi-Fi password"));
  page.part("<input name=password type=password maxlength=64 autocomplete=new-password></label>");
  page.part(label("<button>连接 Wi-Fi</button></form>", "<button>Connect Wi-Fi</button></form>"));
  page.part(R"html(<script>{
const list=document.getElementById('net'),button=document.getElementById('scan'),status=document.getElementById('scanStatus');
const en=document.documentElement.lang==='en',msg=(zh,enText)=>en?enText:zh;
const select=()=>list.replaceChildren(new Option(msg('选择网络或手动输入','Choose a network or enter it manually'),''));
select();
async function scan(){
  button.disabled=true;select();status.textContent=msg('正在扫描…','Scanning…');
  try{
    for(let i=0;i<25;i++){
      const r=await fetch('/scan'+(i?'?poll=1':''),{cache:'no-store'});
      if(r.status===202){await new Promise(done=>setTimeout(done,600));continue}
      if(!r.ok)throw Error();
      const lines=(await r.text()).trim().split('\n').filter(Boolean);
      for(const line of lines){try{const ssid=decodeURIComponent(line);list.add(new Option(ssid,ssid))}catch{}}
      status.textContent=list.options.length>1?'':msg('未找到网络，可手动输入。','No networks found. Enter the name manually.');
      return;
    }
    throw Error();
  }catch{status.textContent=msg('扫描失败，可重试或手动输入。','Scan failed. Retry or enter the name manually.')}
  finally{button.disabled=false}
}
button.onclick=scan;
)html");
  if (autoScan) page.part("scan();");
  page.part("}</script>");
}

void showZoneForm(HtmlPage &page, bool applyImmediately) {
  const char *zone = profile.zone[0] ? profile.zone : defaultZone;
  beginForm(page, "/zone");
  page.part(label("<label>时区", "<label>Time zone"));
  page.part(applyImmediately ? "<select name=zone id=zone required onchange=this.form.requestSubmit()>"
                             : "<select name=zone id=zone required>");
  page.part(String("<option value=\"") + zone + "\" selected>" +
            (strcmp(zone, defaultZone) == 0 ? label("UTC+08:00 · 北京", "UTC+08:00 · Beijing") : zone) + "</option>");
  if (strcmp(zone, "UTC") != 0) page.part("<option value=UTC>UTC</option>");
  if (strcmp(zone, defaultZone) != 0)
    page.part(label("<option value='China Standard Time'>UTC+08:00 · 北京</option>",
                    "<option value='China Standard Time'>UTC+08:00 · Beijing</option>"));
  page.part("</select></label>");
  if (!applyImmediately) page.part(label("<button>保存时区</button>", "<button>Save time zone</button>"));
  page.part("</form>");
  page.part(R"html(<script>
fetch('/zones').then(r=>{if(!r.ok)throw Error();return r.text()}).then(t=>{
  const z=document.getElementById('zone'),v=z.value,old=z.innerHTML;
  z.innerHTML=t;
  const en=document.documentElement.lang==='en';
  for(const o of z.options){const labels=o.textContent.split(' / ');if(labels.length===2)o.textContent=labels[en?1:0]}
  z.value=v;if(z.selectedIndex<0)z.innerHTML=old;
}).catch(()=>{})
</script>)html");
}

void showClockForm(HtmlPage &page) {
  beginForm(page, "/clock");
  page.part("<label class=check><input type=checkbox name=hour24 value=1 onchange=this.form.requestSubmit()");
  if (read24Hour()) page.part(" checked");
  page.part(label(">24 小时制</label></form>",
                  ">24-hour time</label></form>"));
}

void showPage() {
  if (!allowedClient()) return;
  HtmlPage page;
  page.open();
  if (saveFailed)
    page.part(label("<p role=alert>配置保存失败，请重试。</p>", "<p role=alert>Could not save settings. Try again.</p>"));
  switch (stage) {
    case Stage::Setup:
      if (stationFailed)
        page.part(label("<p role=alert>连接失败，请检查 Wi-Fi 名称和密码。设备使用 2.4 GHz Wi-Fi。</p>", "<p role=alert>Connection failed. Check the network name and password. Use 2.4 GHz Wi-Fi.</p>"));
      page.part("<section class=\"card setup\">");
      showNetworkForm(page, true);
      page.part("</section>");
      break;
    case Stage::ChooseZone:
      page.part(label("<p>Wi-Fi 已连接。保存时区后，设备将关闭配网热点并保持联网。</p>", "<p>Wi-Fi connected. Saving the time zone closes the setup hotspot.</p>"));
      {
        const String ip = WiFi.localIP().toString();
        page.part(String(label("<p>设备地址 ", "<p>Device address ")) + "<a href=\"http://" + ip + "/?lang=" + label("zh", "en") + "\">" + ip + "</a></p>");
      }
      page.part("<section class=\"card setup\">");
      showZoneForm(page, false);
      page.part("</section>");
      break;
    case Stage::Complete:
    case Stage::Online: {
      page.part("<div class=status>");
      const String ip = WiFi.localIP().toString();
      page.part(String(label("<p>设备地址 ", "<p>Device address ")) + "<a href=\"http://" + ip + "/?lang=" + label("zh", "en") + "\">" + ip + "</a></p>");
      std::tm clock{};
      if (localTime(clock)) {
        char stamp[28];
        std::strftime(stamp, sizeof(stamp), read24Hour() ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %I:%M:%S %p", &clock);
        page.part(String(label("<p>设备时间 <time>", "<p>Device time <time>")) + stamp + "</time></p>");
      } else {
        page.part(label("<p>正在等待网络时间同步。</p>", "<p>Waiting for network time synchronization.</p>"));
      }
      page.part("</div>");
      if (stage == Stage::Complete && portalClient()) {
        page.part(label("<p>配网完成。热点即将关闭，请使用局域网地址访问设置。</p>", "<p>Setup complete. The hotspot will close. Use the LAN address to open settings.</p>"));
        page.part("<script>setTimeout(()=>window.close(),1200)</script>");
      } else {
        page.part(label("<div class=settings><section class=card><div class=card-head><h2>时钟</h2>",
                        "<div class=settings><section class=card><div class=card-head><h2>Clock</h2>"));
        showClockForm(page);
        page.part("</div>");
        showZoneForm(page, true);
        page.part("</section><section class=card>");
        page.part(String(label("<h2>重新配网</h2><p class=hint>查看进度请连接热点 ",
                               "<h2>Change Wi-Fi</h2><p class=hint>Follow progress from hotspot ")) + apName + "</p>");
        showNetworkForm(page, false);
        page.part("</section></div>");
      }
      break;
    }
    default:
      page.part(label("<p role=status>正在连接 Wi-Fi，请保持连接。</p>", "<p role=status>Connecting to Wi-Fi. Stay connected.</p>"));
      page.part("<script>setTimeout(()=>location.reload(),1500)</script>");
      break;
  }
  page.close();
}

bool acceptForm() {
  if (!allowedClient()) return false;
  if (http.arg("token") != formToken) {
    http.send(400, "text/plain; charset=utf-8", label("页面已过期，请返回设置页面重试。", "Page expired. Return to settings and try again."));
    return false;
  }
  return true;
}

void showTimeZones() {
  if (!allowedClient()) return;
  char header[240];
  const int size = snprintf(header, sizeof(header),
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\n"
      "Content-Length: %u\r\nCache-Control: max-age=3600\r\nConnection: close\r\n\r\n",
      static_cast<unsigned>(sizeof(timeZoneOptionsGzip)));
  PageOutput output;
  output.write(header, size);
  if (output.writable)
    output.write(reinterpret_cast<const char *>(timeZoneOptionsGzip), sizeof(timeZoneOptionsGzip));
}

void redirectHome() {
  http.sendHeader("Location", label("/?lang=zh", "/?lang=en"));
  http.send(303, "text/plain", "");
}

void stopScan() {
  if (!scanPending) return;
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) esp_wifi_scan_stop();
  WiFi.scanDelete();
  scanPending = false;
}

void showNetworks() {
  if (!allowedClient()) return;
  http.sendHeader("Cache-Control", "no-store");
  if (stage != Stage::Setup && stage != Stage::Online) {
    http.send(409, "text/plain", "");
    return;
  }
  if (!scanPending) {
    if (http.hasArg("poll") || WiFi.scanNetworks(true, false, false, 120) != WIFI_SCAN_RUNNING) {
      http.send(503, "text/plain", "");
      return;
    }
    scanPending = true;
    scanStarted = millis();
  }
  const int count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) {
    http.send(202, "text/plain", "");
    return;
  }
  String result;
  constexpr char hex[] = "0123456789ABCDEF";
  for (int i = 0, added = 0; i < count && added < 12; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    char line[98];
    size_t size = 0;
    for (uint8_t c : ssid) {
      line[size++] = '%';
      line[size++] = hex[c >> 4];
      line[size++] = hex[c & 15];
    }
    line[size++] = '\n';
    line[size] = '\0';
    int match = result.indexOf(line);
    while (match > 0 && result[match - 1] != '\n') match = result.indexOf(line, match + 1);
    if (match < 0) {
      result += line;
      ++added;
    }
  }
  stopScan();
  http.send(count >= 0 ? 200 : 503, "text/plain", result);
}

void submitClock() {
  if (!acceptForm()) return;
  if (stage != Stage::Online) {
    http.send(409, "text/plain", "");
    return;
  }
  write24Hour(http.arg("hour24") == "1");
  redirectHome();
}

void joinNetwork(uint32_t now) {
  WiFi.setAutoReconnect(false);
  WiFi.begin(profile.ssid, profile.password);
  WiFi.setSleep(false);
  Serial.printf("Wi-Fi: joining network (heap=%lu)\n", static_cast<unsigned long>(ESP.getFreeHeap()));
  stage = Stage::Joining;
  deadline = now + connectTimeout;
}

bool startPortal();
void stopSync();

void submitNetwork() {
  if (!acceptForm()) return;
  if (stage != Stage::Setup && stage != Stage::Online) {
    http.send(409, "text/plain; charset=utf-8", label("正在配置网络，请稍后重试。", "Network setup is in progress. Try again shortly."));
    return;
  }
  const String ssid = http.arg("ssid");
  const String password = http.arg("password");
  const bool hexKey = password.length() == 64 && strspn(password.c_str(), "0123456789abcdefABCDEF") == 64;
  if (ssid.isEmpty() || ssid.length() > 32 || ssid.length() != strlen(ssid.c_str()) ||
      password.length() != strlen(password.c_str()) ||
      (!password.isEmpty() && !hexKey && (password.length() < 8 || password.length() > 63))) {
    http.send(400, "text/plain; charset=utf-8", label("Wi-Fi 名称或密码格式有误。", "Invalid Wi-Fi name or password."));
    return;
  }
  if (!portalActive && !startPortal()) {
    http.send(503, "text/plain; charset=utf-8", label("无法打开配网热点，请稍后重试。", "Could not start the setup hotspot. Try again."));
    return;
  }
  stopScan();
  stopSync();
  profile = {};
  ssid.toCharArray(profile.ssid, sizeof(profile.ssid));
  password.toCharArray(profile.password, sizeof(profile.password));
  stationFailed = saveFailed = false;
  redirectHome();
  if (stationConnected()) WiFi.disconnect();
  joinNetwork(millis());
}

void stopSync() {
  if (!sntpActive) return;
  esp_netif_sntp_deinit();
  sntpActive = false;
}

void startSync() {
  if (sntpActive) return;
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
      3, ESP_SNTP_SERVER_LIST("pool.ntp.org", "ntp.aliyun.com", "time.cloudflare.com"));
  sntpActive = esp_netif_sntp_init(&config) == ESP_OK;
}

void submitZone() {
  if (!acceptForm()) return;
  if (!stationConnected() || (stage != Stage::ChooseZone && stage != Stage::Online)) {
    http.send(409, "text/plain; charset=utf-8", label("请先完成 Wi-Fi 连接。", "Connect to Wi-Fi first."));
    return;
  }
  const String zone = http.arg("zone");
  if (zone.isEmpty() || zone.length() >= sizeof(profile.zone) ||
      zone.length() != strlen(zone.c_str()) || !knownZone(zone.c_str())) {
    http.send(400, "text/plain; charset=utf-8", label("请选择有效的时区。", "Choose a valid time zone."));
    return;
  }
  char previousZone[sizeof(profile.zone)];
  memcpy(previousZone, profile.zone, sizeof(previousZone));
  zone.toCharArray(profile.zone, sizeof(profile.zone));
  saveFailed = !saveProfile();
  if (saveFailed) {
    memcpy(profile.zone, previousZone, sizeof(previousZone));
  } else {
    initialSetup = false;
    startSync();
    if (portalActive) {
      stage = Stage::Complete;
      deadline = millis() + 7000;
    }
    Serial.printf("Wi-Fi settings: time zone saved (%s)\n", profile.zone);
  }
  redirectHome();
}

void startHttp() {
  if (httpActive) return;
  http.begin();
  httpActive = true;
}

bool startPortal() {
  const wifi_mode_t mode = stage == Stage::Online ? WIFI_AP_STA : WIFI_AP;
  if (!WiFi.mode(mode) || !WiFi.softAP(apName, nullptr, 1, 0, 1)) {
    WiFi.mode(stage == Stage::Online ? WIFI_STA : WIFI_OFF);
    return false;
  }
  dns.start(53, "*", WiFi.softAPIP());
  startHttp();
  portalActive = true;
  Serial.printf("Wi-Fi setup: %s at http://%s/ (heap=%lu)\n", apName, WiFi.softAPIP().toString().c_str(),
                static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

void reconnectAfterSetup(uint32_t now) {
  // Tear down the temporary AP+STA driver before the normal STA connection.
  // Release the portal's TCP/DNS allocations before reconnecting.
  stopSync();
  dns.stop();
  http.stop();
  portalActive = httpActive = false;
  WiFi.mode(WIFI_OFF);
  stage = Stage::Start;
  deadline = now + 250;
}

void completeSetup() {
  WiFi.setAutoReconnect(true);
  stage = Stage::Online;
  Serial.printf("Wi-Fi settings: http://%s/ (heap=%lu)\n",
                WiFi.localIP().toString().c_str(), static_cast<unsigned long>(ESP.getFreeHeap()));
}
} // namespace

void begin(const char *deviceName, bool (*get24Hour)(), void (*set24Hour)(bool)) {
  read24Hour = get24Hour;
  write24Hour = set24Hour;
  Preferences storage;
  if (storage.begin(settingsNamespace, true) && storage.getBytesLength(settingsKey) == sizeof(profile))
    storage.getBytes(settingsKey, &profile, sizeof(profile));
  profile.ssid[32] = profile.password[64] = profile.zone[63] = '\0';
  if (!knownZone(profile.zone)) profile.zone[0] = '\0';

  WiFi.persistent(false);
  const uint32_t tokenHigh = esp_random();
  const uint32_t tokenLow = esp_random();
  snprintf(formToken, sizeof(formToken), "%08lx%08lx",
           static_cast<unsigned long>(tokenHigh), static_cast<unsigned long>(tokenLow));
  strlcpy(apName, deviceName, sizeof(apName));
  initialSetup = !profile.ssid[0] || !profile.zone[0];
  http.on("/", HTTP_GET, showPage);
  http.on("/zones", HTTP_GET, showTimeZones);
  http.on("/scan", HTTP_GET, showNetworks);
  http.on("/clock", HTTP_POST, submitClock);
  http.on("/wifi", HTTP_POST, submitNetwork);
  http.on("/zone", HTTP_POST, submitZone);
  http.onNotFound([] {
    if (!allowedClient()) return;
    if (portalClient()) {
      http.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
      http.send(302, "text/plain", "");
    } else {
      http.send(404, "text/plain", "");
    }
  });
  deadline = millis();
}

void loop() {
  if (scanPending && millis() - scanStarted >= 15000) stopScan();
  if (initialSetup && millis() >= setupWindow) {
    stopScan();
    stopSync();
    if (portalActive) dns.stop();
    if (httpActive) http.stop();
    WiFi.mode(WIFI_OFF);
    initialSetup = portalActive = httpActive = false;
    profile = {};
    stage = Stage::Unconfigured;
    Serial.println("Wi-Fi setup: closed after 10 minutes without configuration");
    return;
  }
  if (httpActive) http.handleClient();
  const uint32_t now = millis();
  if (sntpActive && esp_netif_sntp_sync_wait(0) == ESP_OK && time(nullptr) >= earliestTime) {
    clockReady = true;
    Serial.println("NTP: synchronized");
  }
  switch (stage) {
    case Stage::Start:
      if (!expired(now)) break;
      if (profile.zone[0] && profile.ssid[0]) {
        WiFi.mode(WIFI_STA);
        joinNetwork(now);
      } else if (startPortal()) {
        stage = Stage::Setup;
      } else {
        deadline = now + retryDelay;
      }
      break;
    case Stage::Joining:
      if (stationConnected()) {
        Serial.printf("Wi-Fi: connected at %s (channel=%d, heap=%lu)\n", WiFi.localIP().toString().c_str(),
                      WiFi.channel(), static_cast<unsigned long>(ESP.getFreeHeap()));
        if (portalActive) {
          stage = Stage::ChooseZone;
        } else {
          startHttp();
          startSync();
          completeSetup();
        }
      } else if (expired(now)) {
        Serial.printf("Wi-Fi: association timed out (status=%d)\n", WiFi.status());
        WiFi.disconnect();
        stationFailed = true;
        if (portalActive) {
          stage = Stage::Setup;
        } else {
          stage = Stage::Start;
          deadline = now + retryDelay;
        }
      }
      break;
    case Stage::ChooseZone:
      if (!stationConnected()) {
        stationFailed = true;
        stage = Stage::Setup;
      }
      break;
    case Stage::Complete:
      if (expired(now)) reconnectAfterSetup(now);
      break;
    case Stage::Online:
    case Stage::Setup:
    case Stage::Unconfigured:
      break;
  }
}

bool localTime(std::tm &result) {
  if (!clockReady) return false;
  const time_t now = time(nullptr);
  char rule[64];
  if (!bridge::timezone::resolve(profile.zone, now, rule, sizeof(rule))) return false;
  static char previousRule[sizeof(rule)] = "";
  if (strcmp(previousRule, rule) != 0) {
    if (setenv("TZ", rule, 1) != 0) return false;
    tzset();
    strcpy(previousRule, rule);
  }
  return localtime_r(&now, &result) != nullptr;
}

bool clearSettings() {
  Preferences storage;
  return storage.begin(settingsNamespace) && storage.clear();
}
} // namespace clockwiseWifi
#endif
