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
// ===== imole_crypto (inlined for single-file Wokwi use) =====
// imole_crypto.h - Ethereum-style crypto for the Imole ESP32 meter.
// keccak256 + secp256k1 ECDSA with recovery id (v), built on the mbedtls that ships with the
// ESP32 Arduino core (2.x and 3.x). Header-only so it drops into a Wokwi/Arduino sketch.
// Tested on a PC against ethers.js (see test_crypto.c in this folder).
#include <stdint.h>
#include <string.h>
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

#ifndef MBEDTLS_PRIVATE
#define MBEDTLS_PRIVATE(member) member
#endif

// ------------------------------------------------------------------ keccak256 (Ethereum)
static const uint64_t IM_RC[24] = {
  0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
  0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
  0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
  0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
  0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
  0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
static const int IM_ROT[25] = {0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14};

static inline uint64_t im_rol(uint64_t x, int s) { return s ? (x << s) | (x >> (64 - s)) : x; }

static void im_keccakf(uint64_t A[25]) {
  for (int round = 0; round < 24; round++) {
    uint64_t C[5], D[5], B[25];
    for (int x = 0; x < 5; x++) C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
    for (int x = 0; x < 5; x++) D[x] = C[(x + 4) % 5] ^ im_rol(C[(x + 1) % 5], 1);
    for (int i = 0; i < 25; i++) A[i] ^= D[i % 5];
    for (int x = 0; x < 5; x++)
      for (int y = 0; y < 5; y++) B[y + 5 * ((2 * x + 3 * y) % 5)] = im_rol(A[x + 5 * y], IM_ROT[x + 5 * y]);
    for (int x = 0; x < 5; x++)
      for (int y = 0; y < 5; y++) A[x + 5 * y] = B[x + 5 * y] ^ ((~B[(x + 1) % 5 + 5 * y]) & B[(x + 2) % 5 + 5 * y]);
    A[0] ^= IM_RC[round];
  }
}

static void keccak256(const uint8_t* in, size_t len, uint8_t out[32]) {
  uint64_t A[25];
  memset(A, 0, sizeof(A));
  const size_t rate = 136;
  uint8_t block[136];
  while (len >= rate) {
    for (int i = 0; i < 17; i++) { uint64_t w = 0; for (int b = 0; b < 8; b++) w |= (uint64_t)in[i * 8 + b] << (8 * b); A[i] ^= w; }
    im_keccakf(A); in += rate; len -= rate;
  }
  memset(block, 0, rate);
  memcpy(block, in, len);
  block[len] = 0x01;            // Keccak (not SHA3) padding
  block[rate - 1] |= 0x80;
  for (int i = 0; i < 17; i++) { uint64_t w = 0; for (int b = 0; b < 8; b++) w |= (uint64_t)block[i * 8 + b] << (8 * b); A[i] ^= w; }
  im_keccakf(A);
  for (int i = 0; i < 4; i++) for (int b = 0; b < 8; b++) out[i * 8 + b] = (uint8_t)(A[i] >> (8 * b));
}

// ------------------------------------------------------------------ secp256k1
static const char* IM_N_HEX = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141";


// Ethereum address (20 bytes) for a 32-byte private key. Returns 0 on success.
static int eth_address(const uint8_t priv[32], uint8_t addr[20], int (*rng)(void*, unsigned char*, size_t), void* rng_ctx) {
  mbedtls_ecp_group grp; mbedtls_ecp_point Q; mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q); mbedtls_mpi_init(&d);
  int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1);
  if (!ret) ret = mbedtls_mpi_read_binary(&d, priv, 32);
  if (!ret) ret = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, rng, rng_ctx);
  uint8_t pub[65]; size_t olen = 0;
  if (!ret) ret = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, sizeof(pub));
  if (!ret) { uint8_t h[32]; keccak256(pub + 1, 64, h); memcpy(addr, h + 12, 20); }
  mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&Q); mbedtls_mpi_free(&d);
  return ret;
}

// Sign a 32-byte digest. Writes r(32) || s(32) || v(1) with v = 27/28 and low-s (EIP-2).
static int eth_sign(const uint8_t priv[32], const uint8_t digest[32], uint8_t sig[65], int (*rng)(void*, unsigned char*, size_t), void* rng_ctx) {
  mbedtls_ecp_group grp; mbedtls_ecp_point R; mbedtls_mpi d, k, z, r, s, n, halfN, kinv, t;
  mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&R);
  mbedtls_mpi* all[] = {&d, &k, &z, &r, &s, &n, &halfN, &kinv, &t};
  for (auto m : all) mbedtls_mpi_init(m);
  int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1);
  if (!ret) ret = mbedtls_mpi_read_string(&n, 16, IM_N_HEX);
  if (!ret) ret = mbedtls_mpi_copy(&halfN, &n);
  if (!ret) ret = mbedtls_mpi_shift_r(&halfN, 1);
  if (!ret) ret = mbedtls_mpi_read_binary(&d, priv, 32);
  if (!ret) ret = mbedtls_mpi_read_binary(&z, digest, 32);
  int v = 0;
  for (int tries = 0; !ret && tries < 16; tries++) {
    // k random in [1, n-1]; R = k*G
    ret = mbedtls_ecp_gen_privkey(&grp, &k, rng, rng_ctx);
    if (!ret) ret = mbedtls_ecp_mul(&grp, &R, &k, &grp.G, rng, rng_ctx);
    if (ret) break;
    uint8_t rp[65]; size_t olen = 0;
    ret = mbedtls_ecp_point_write_binary(&grp, &R, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, rp, sizeof(rp));
    if (ret) break;
    int yOdd = rp[64] & 1;
    ret = mbedtls_mpi_read_binary(&r, rp + 1, 32);            // r = R.x
    int xOverflow = mbedtls_mpi_cmp_mpi(&r, &n) >= 0;
    if (!ret) ret = mbedtls_mpi_mod_mpi(&r, &r, &n);
    if (ret || mbedtls_mpi_cmp_int(&r, 0) == 0) continue;
    // s = k^-1 (z + r*d) mod n
    ret = mbedtls_mpi_mul_mpi(&t, &r, &d);
    if (!ret) ret = mbedtls_mpi_add_mpi(&t, &t, &z);
    if (!ret) ret = mbedtls_mpi_mod_mpi(&t, &t, &n);
    if (!ret) ret = mbedtls_mpi_inv_mod(&kinv, &k, &n);
    if (!ret) ret = mbedtls_mpi_mul_mpi(&s, &kinv, &t);
    if (!ret) ret = mbedtls_mpi_mod_mpi(&s, &s, &n);
    if (ret || mbedtls_mpi_cmp_int(&s, 0) == 0) continue;
    v = yOdd | (xOverflow << 1);
    if (mbedtls_mpi_cmp_mpi(&s, &halfN) > 0) {                // low-s: s = n - s flips parity
      ret = mbedtls_mpi_sub_mpi(&s, &n, &s);
      v ^= 1;
    }
    if (!ret) ret = mbedtls_mpi_write_binary(&r, sig, 32);
    if (!ret) ret = mbedtls_mpi_write_binary(&s, sig + 32, 32);
    sig[64] = (uint8_t)(27 + v);
    break;
  }
  mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&R);
  for (auto m : all) mbedtls_mpi_free(m);
  return ret;
}

// ------------------------------------------------------------------ hex helpers
static void im_hex(const uint8_t* b, size_t n, char* out) {  // out needs 2n+1
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
  out[2 * n] = 0;
}
static int im_unhex(const char* hex, uint8_t* out, size_t n) {
  if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
  for (size_t i = 0; i < n; i++) {
    int v = 0;
    for (int j = 0; j < 2; j++) {
      char c = hex[2 * i + j]; v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return -1;
    }
    out[i] = (uint8_t)v;
  }
  return 0;
}
// 32-byte big-endian word holding a uint64
static void im_word_u64(uint64_t x, uint8_t w[32]) { memset(w, 0, 32); for (int i = 0; i < 8; i++) w[31 - i] = (uint8_t)(x >> (8 * i)); }

// EIP-712 digest for ImoleGrid Usage(bytes32 meterId,uint64 wh,uint64 nonce)
static void imole_usage_digest(const uint8_t domainSep[32], const uint8_t meterId[32], uint64_t wh, uint64_t nonce, uint8_t out[32]) {
  static const char* TYPE = "Usage(bytes32 meterId,uint64 wh,uint64 nonce)";
  uint8_t buf[128];
  keccak256((const uint8_t*)TYPE, strlen(TYPE), buf);
  memcpy(buf + 32, meterId, 32);
  im_word_u64(wh, buf + 64);
  im_word_u64(nonce, buf + 96);
  uint8_t structHash[32];
  keccak256(buf, 128, structHash);
  uint8_t msg[66];
  msg[0] = 0x19; msg[1] = 0x01;
  memcpy(msg + 2, domainSep, 32);
  memcpy(msg + 34, structHash, 32);
  keccak256(msg, 66, out);
}

// ===== end imole_crypto =====

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
