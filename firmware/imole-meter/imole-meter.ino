/*
  Imole meter (ESP32) for ImoleGrid on BNB Chain - Agent ID edition

  - Reads the meter's prepaid credit from the ImoleGrid contract every POLL_MS.
  - Closes the relay (load ON) while credit remains, opens it at zero.
  - Meters energy: the potentiometer stands in for the INA219 sensor (0-100 W load).
  - Every REPORT_MS it signs Usage{meterId, wh, nonce} (EIP-712, secp256k1) with the DEVICE KEY
    that lives only on this board, and posts it to the relayer, which pays gas and submits it.
    The contract checks the signature and pays the installer for exactly that energy.
  - LCD shows the owner's .ai name, kWh left, load and relay state.

  Wiring (same as diagram.json):
    GPIO 26 -> relay IN        GPIO 2 -> green LED (power on)   GPIO 4 -> red LED (no credit)
    GPIO 34 <- potentiometer (load watts)     GPIO 15 <- "Internet" slide switch (to GND = off)
    SDA 21 / SCL 22 -> 16x2 I2C LCD at 0x27
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#if __has_include("esp_random.h")
#include "esp_random.h"
#else
#include "esp_system.h"
#endif
#include "imole_crypto.h"

// ---------------------------------------------------------------- configure me
const char* WIFI_SSID = "Wokwi-GUEST";   // your Wi-Fi on real hardware
const char* WIFI_PASS = "";
const char* GRID = "0xcc3A2b650a904a64AfDcD6a3319056AAC8BBcc99";               // ImoleGrid v2, BSC mainnet
const char* REGISTRY = "0x60B9262456dE011F7d15a54CdD8c48bAaa8B022F";           // Agent ID registry
const char* METER_ID = "8585db12c1df78cc5d76e7bcdb56e3f44262e21844fe0cb640b399c4a42045e4"; // keccak("imole:ife-001")
const char* RELAY_URL = "https://imole-energy.vercel.app/api/report";
// Device key: 64 hex chars. Leave EMPTY to generate a fresh key at boot (it prints the address;
// the operator then calls setDevice). On a real board, generate once and keep it in flash only.
// Never commit a real key to a public repo or a public Wokwi project.
const char* DEVICE_KEY_HEX = "";
const float MAX_LOAD_W = 100.0;   // potentiometer full scale
const float SPEED = 60.0;         // demo time-lapse: 1 real minute = 1 hour of use. Use 1 for real time.
const uint32_t POLL_MS = 10000;
const uint32_t REPORT_MS = 60000;
// ------------------------------------------------------------------------------

const char* RPCS[] = {"https://bsc-rpc.publicnode.com", "https://bsc-dataseed.bnbchain.org"};
const int PIN_RELAY = 26, PIN_LED_ON = 2, PIN_LED_OFF = 4, PIN_POT = 34, PIN_NET = 15;

LiquidCrystal_I2C lcd(0x27, 16, 2);
uint8_t devKey[32], meterId[32], domainSep[32];
char devAddr[43];
bool haveDomain = false;

// chain state (written by the network task, read by loop)
volatile bool chainOk = false;
volatile uint64_t creditWh = 0, lastNonce = 0;
volatile bool meterActive = false;
volatile uint32_t lastChainMs = 0;
char ownerName[24] = "Imole meter";
char onchainDevice[43] = "";

// local metering
double unsettledWh = 0;
uint32_t lastTickMs = 0, lastReportMs = 0, lastDrawMs = 0;
volatile bool reportBusy = false;
String lastMsg = "";

static int rngFn(void*, unsigned char* out, size_t n) { esp_fill_random(out, n); return 0; }
bool online() { return digitalRead(PIN_NET) == HIGH && WiFi.status() == WL_CONNECTED; }
float loadW() { return MAX_LOAD_W * analogRead(PIN_POT) / 4095.0; }

// ---------------------------------------------------------------- HTTP / JSON-RPC
String httpPost(const char* url, const String& body) {
  WiFiClientSecure tls;
  tls.setInsecure();  // demo shortcut; pin the CA on production hardware
  HTTPClient http;
  if (!http.begin(tls, url)) return "";
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();
  return resp;
}

String jsonField(const String& resp, const char* key) {
  String k = String("\"") + key + "\":\"";
  int at = resp.indexOf(k);
  if (at < 0) return "";
  return resp.substring(at + k.length(), resp.indexOf('"', at + k.length()));
}

String ethCall(const char* to, const String& data) {
  String body = String("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"eth_call\",\"params\":[{\"to\":\"") + to +
                "\",\"data\":\"" + data + "\"},\"latest\"]}";
  for (int i = 0; i < 2; i++) {
    String r = jsonField(httpPost(RPCS[i], body), "result");
    if (r.length() >= 2) return r;
  }
  return "";
}

uint64_t wordU64(const String& hex, int idx) {
  int start = 2 + idx * 64;
  if ((int)hex.length() < start + 64) return 0;
  return strtoull(hex.substring(start + 48, start + 64).c_str(), nullptr, 16);
}
String wordAddr(const String& hex, int idx) { int s = 2 + idx * 64; return "0x" + hex.substring(s + 24, s + 64); }

String decodeString(const String& hex) {  // ABI-encoded single string return
  if (hex.length() < 2 + 128) return "";
  int len = (int)wordU64(hex, 1);
  String out;
  for (int i = 0; i < len && i < 22; i++) out += (char)strtol(hex.substring(2 + 128 + 2 * i, 2 + 128 + 2 * i + 2).c_str(), nullptr, 16);
  return out;
}

// ---------------------------------------------------------------- chain task
void readChain() {
  if (!haveDomain) {
    String d = ethCall(GRID, "0xf698da25");  // domainSeparator()
    if (d.length() == 66 && im_unhex(d.c_str(), domainSep, 32) == 0) haveDomain = true;
  }
  String m = ethCall(GRID, String("0x1850861f") + METER_ID);  // meters(bytes32)
  if (m.length() < 2 + 64 * 10) { Serial.println("[chain] read failed"); return; }
  String device = wordAddr(m, 1), owner = wordAddr(m, 2);
  creditWh = wordU64(m, 6);
  lastNonce = wordU64(m, 7);
  meterActive = wordU64(m, 9) != 0;
  device.toCharArray(onchainDevice, sizeof(onchainDevice));
  static String knownOwner = "";
  if (owner != knownOwner && owner != "0x0000000000000000000000000000000000000000") {
    String h = decodeString(ethCall(REGISTRY, String("0xc50a1514000000000000000000000000") + owner.substring(2)));
    if (h.length()) { (h + ".ai").toCharArray(ownerName, sizeof(ownerName)); knownOwner = owner; }
  }
  chainOk = true;
  lastChainMs = millis();
  Serial.printf("[chain] credit %llu Wh · nonce %llu · %s · owner %s\n", creditWh, lastNonce, meterActive ? "active" : "paused", ownerName);
}

bool sendReport(uint64_t wh, uint64_t nonce) {
  if (!haveDomain) return false;
  uint8_t digest[32], sig[65];
  imole_usage_digest(domainSep, meterId, wh, nonce, digest);
  if (eth_sign(devKey, digest, sig, rngFn, nullptr) != 0) { Serial.println("[sign] failed"); return false; }
  char sigHex[131]; im_hex(sig, 65, sigHex);
  String body = String("{\"meterId\":\"0x") + METER_ID + "\",\"wh\":" + String((unsigned long)wh) +
                ",\"nonce\":" + String((unsigned long)nonce) + ",\"signature\":\"0x" + sigHex + "\"}";
  String resp = httpPost(RELAY_URL, body);
  Serial.printf("[report] %llu Wh nonce %llu -> %s\n", wh, nonce, resp.c_str());
  return resp.indexOf("\"ok\":true") >= 0;
}

volatile uint64_t pendingReportWh = 0;
volatile bool reportResult = false, reportDone = false;

void netTask(void*) {
  uint32_t lastPoll = 0;
  for (;;) {
    if (online()) {
      if (pendingReportWh > 0 && !reportDone) {
        uint64_t wh = pendingReportWh;
        reportResult = sendReport(wh, lastNonce + 1);
        if (reportResult) { lastNonce = lastNonce + 1; creditWh = creditWh > wh ? creditWh - wh : 0; }
        reportDone = true;
        lastPoll = 0;  // re-read soon after a report
      } else if (lastPoll == 0 || millis() - lastPoll >= POLL_MS) {
        lastPoll = millis();
        readChain();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---------------------------------------------------------------- UI
void pad16(const String& s) { String t = s.substring(0, 16); while (t.length() < 16) t += ' '; lcd.print(t); }

void draw(bool on) {
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
  digitalWrite(PIN_LED_ON, on ? HIGH : LOW);
  digitalWrite(PIN_LED_OFF, on ? LOW : HIGH);
  double left = (double)creditWh - unsettledWh; if (left < 0) left = 0;
  String name = String(ownerName); if (name.length() > 12) name = name.substring(0, 12);
  String top = name; while (top.length() < 13) top += ' '; top += on ? " ON" : "OFF";
  char bottom[24];
  if (!chainOk) snprintf(bottom, sizeof(bottom), "%s", online() ? "Reading chain.." : "No internet");
  else if (lastMsg.length()) snprintf(bottom, sizeof(bottom), "%s", lastMsg.c_str());
  else snprintf(bottom, sizeof(bottom), "%.3fkWh %3.0fW", left / 1000.0, on ? loadW() : 0.0);
  lcd.setCursor(0, 0); pad16(top);
  lcd.setCursor(0, 1); pad16(String(bottom));
}

// ---------------------------------------------------------------- main
void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY, OUTPUT); pinMode(PIN_LED_ON, OUTPUT); pinMode(PIN_LED_OFF, OUTPUT);
  pinMode(PIN_NET, INPUT_PULLUP);
  digitalWrite(PIN_RELAY, LOW);
  lcd.init(); lcd.backlight();
  lcd.setCursor(0, 0); pad16("Imole  BNB Chain"); lcd.setCursor(0, 1); pad16("Starting...");

  im_unhex(METER_ID, meterId, 32);
  if (strlen(DEVICE_KEY_HEX) == 64) im_unhex(DEVICE_KEY_HEX, devKey, 32);
  else { esp_fill_random(devKey, 32); Serial.println("[key] no DEVICE_KEY_HEX set - generated a temporary key for this run"); }
  uint8_t addr[20]; eth_address(devKey, addr, rngFn, nullptr);
  devAddr[0] = '0'; devAddr[1] = 'x'; im_hex(addr, 20, devAddr + 2);
  Serial.printf("[key] device address %s\n", devAddr);
  lcd.setCursor(0, 1); pad16(String("Dev ") + String(devAddr).substring(0, 6) + ".." + String(devAddr).substring(38));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  Serial.printf("[wifi] %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "offline");
  xTaskCreatePinnedToCore(netTask, "net", 24576, nullptr, 1, nullptr, 1);
  lastTickMs = lastReportMs = millis();
}

void loop() {
  uint32_t now = millis();
  bool fresh = chainOk && (now - lastChainMs < 10UL * 60 * 1000);       // stale data -> fail safe (off)
  bool keyMatches = strcasecmp(onchainDevice, devAddr) == 0;
  double left = (double)creditWh - unsettledWh;
  bool on = fresh && meterActive && left > 0;

  if (chainOk && !keyMatches) lastMsg = "Key!=meter dev";  // operator must call setDevice(devAddr)
  else lastMsg = "";

  // meter energy while the relay is closed
  double dtH = (now - lastTickMs) / 3600000.0 * SPEED;
  lastTickMs = now;
  if (on) unsettledWh += loadW() * dtH;
  if (unsettledWh > creditWh) unsettledWh = creditWh;  // relay opens at zero; can't overdraw

  // hand a report to the network task
  if (reportDone) {
    if (reportResult) { unsettledWh -= (double)pendingReportWh; if (unsettledWh < 0) unsettledWh = 0; lastReportMs = now; }
    pendingReportWh = 0; reportDone = false;
  }
  bool due = (now - lastReportMs >= REPORT_MS) || (!on && unsettledWh >= 1);
  if (keyMatches && fresh && pendingReportWh == 0 && due && unsettledWh >= 1) pendingReportWh = (uint64_t)unsettledWh;

  if (now - lastDrawMs >= 500) { lastDrawMs = now; draw(on); }
  delay(20);
}
