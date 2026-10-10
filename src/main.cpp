/*
 * agri-relay-poe — autonomous single-variable hysteresis relay node on
 *                  M5Stack AtomS3 Lite + Atomic PoE Base (W5500) + Unit Relay.
 *
 * Watches ONE agriha value (MQTT, optional CCM fallback) and drives the M5 Unit
 * Relay with a declarative safety envelope (hysteresis / schedule / gate /
 * dwell / duty / max-on watchdog / fail-safe OFF on stale input). Autonomous:
 * keeps running if the master dies. Generalizes agri-co2-burner to a PoE node
 * whose controlled variable and setpoints are configurable.
 *
 *   control decision + relay drive ............................ control.h
 *   input / gate / remote-command acquisition ................ sources.h
 *   NVS-backed config (CommonConfig + relay rule) ............. config.h
 *   status LED on G35 (AtomS3 Lite, not the core's G27) ...... led.h
 *
 * Hardware / wiring: see README. Include the core headers individually (not
 * <AgriNode.h>) so AgriLED.h's compile-time G27 addLeds never instantiates on
 * the ESP32-S3 — this node drives its own G35 pixel in led.h.
 */
#include <Arduino.h>
#include <time.h>
#include <ArduinoJson.h>
#include <AgriCommonConfig.h>
#include <AgriNetwork.h>
#include <AgriMQTT.h>
#include <AgriOTA.h>
#include <AgriWebUI.h>
#include <AgriProvisionAP.h>

#include "config.h"
#include "led.h"
#include "sources.h"
#include "control.h"

const char *FW_NAME     = "agri-relay-poe";
const char *FW_VERSION  = "0.1.0";
const char *FW_REPO     = "yasunorioi/agri-relay-poe";
const char *FW_BIN_NAME = "agri-relay-poe.bin";

// W5500 pins on the Atomic PoE Base for AtomS3 (see agri-temp-poe); overridable
// via build_flags, fallbacks keep this file self-contained.
#ifndef W5500_SCK
#define W5500_SCK  5
#endif
#ifndef W5500_MISO
#define W5500_MISO 7
#endif
#ifndef W5500_MOSI
#define W5500_MOSI 8
#endif
#ifndef W5500_CS
#define W5500_CS   6
#endif

static const int PIN_BUTTON = 41;   // AtomS3 Lite onboard button (active LOW)

// ---- globals declared extern in the headers --------------------------------
AppConfig   g_cfg;
bool        g_relay     = false;
uint32_t    g_relay_ms  = 0;
const char *g_reason    = "boot";
uint32_t    g_duty_win_start_ms = 0;
uint32_t    g_duty_on_ms        = 0;
uint32_t    g_duty_last_ms      = 0;
float       g_val      = NAN;
uint32_t    g_val_ms   = 0;
const char *g_val_src  = "-";
float       g_gate     = NAN;
uint32_t    g_gate_ms  = 0;

// ---- small HTML / form helpers ---------------------------------------------
static String esc(const char *v) {
  String s(v);
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("'", "&#39;"); s.replace("\"", "&quot;");
  return s;
}
static String inTxt(const String &name, const String &val, const char *type = "text") {
  return "<input type=" + String(type) + " name='" + name + "' value='" + val + "'>";
}
static String inChk(const String &name, bool on) {
  return "<input type=checkbox name='" + name + "'" + (on ? " checked" : "") + ">";
}
static float pfFloat(const String &body, const char *key, float def) {
  char buf[24]; dtostrf(def, 1, 3, buf);
  agri::parseFormStr(body, key, buf, sizeof(buf));
  return atof(buf);
}
static const char *modeStr(uint8_t m) {
  return m == MODE_FORCE_ON ? "FORCE ON" : m == MODE_FORCE_OFF ? "FORCE OFF" : "AUTO";
}

// ---- Dashboard block (hook) ------------------------------------------------
static String renderDashboardSensors() {
  String s; s.reserve(1400);
  // Big relay-state banner
  s  = F("<h3>Relay</h3><div style='font-size:28px;font-weight:bold;padding:10px;border-radius:6px;background:");
  s += g_relay ? "#5a3410;color:#fc6" : "#10301a;color:#7c7";
  s += "'>"; s += g_relay ? "ON" : "OFF";
  s += F("</div><table>");
  s += "<tr><th>Mode</th><td>"; s += modeStr(g_cfg.mode); s += "</td></tr>";
  s += "<tr><th>Reason</th><td>"; s += esc(g_reason ? g_reason : ""); s += "</td></tr>";
  s += "<tr><th>Input (";
  s += esc(g_cfg.input_type); s += ")</th><td>";
  if (valFresh()) { s += String(g_val, 2); s += " <span style='color:#888'>("; s += g_val_src; s += ")</span>"; }
  else            { s += F("<span style='color:#e05252'>stale / none → fail-safe</span>"); }
  s += "</td></tr>";
  s += "<tr><th>Rule</th><td>";
  s += (g_cfg.dir == DIR_RAISE ? "RAISE  ON&le;" : "LOWER  ON&ge;");
  s += String(g_cfg.on_point, 1);
  s += (g_cfg.dir == DIR_RAISE ? ", OFF&ge;" : ", OFF&le;");
  s += String(g_cfg.off_point, 1);
  s += "</td></tr>";
  if (g_cfg.gate_enabled) {
    s += "<tr><th>Gate</th><td>";
    if (gateFresh()) { s += String(g_gate, 1); s += (g_gate <= g_cfg.gate_open_above ? " (closed/ok)" : " (open→suppress)"); }
    else s += F("unknown");
    s += "</td></tr>";
  }
  if (g_cfg.duty_window_s) { s += "<tr><th>Duty</th><td>"; s += dutyPctNow(); s += " % / "; s += g_cfg.duty_pct_max; s += " % max</td></tr>"; }
  s += "<tr><th>Relay pin</th><td>G"; s += g_cfg.relay_pin; s += g_cfg.active_high ? " (active HIGH)" : " (active LOW)"; s += "</td></tr>";
  s += F("</table>");
  return s;
}

// ---- Config rows (hook) ----------------------------------------------------
static String renderConfigSensorRows() {
  String s; s.reserve(3000);
  auto hdr = [&](const char *t) {
    s += F("<tr><th colspan=2 style='background:#1a1a1f;color:#9ad;text-align:left;padding:8px'>");
    s += t; s += F("</th></tr>");
  };
  auto row = [&](const char *label, const String &input) {
    s += "<tr><th>"; s += label; s += "</th><td>"; s += input; s += "</td></tr>";
  };

  hdr("Relay");
  row("Mode", String("<select name='mode'>")
      + "<option value=0" + (g_cfg.mode==MODE_AUTO?" selected":"") + ">AUTO</option>"
      + "<option value=1" + (g_cfg.mode==MODE_FORCE_ON?" selected":"") + ">FORCE ON</option>"
      + "<option value=2" + (g_cfg.mode==MODE_FORCE_OFF?" selected":"") + ">FORCE OFF</option></select>");
  row("Relay DIN pin (GPIO)", inTxt("rpin", String(g_cfg.relay_pin), "number"));
  row("Active HIGH",          inChk("rah", g_cfg.active_high));
  row("Relay id (topic)",     inTxt("rid", esc(g_cfg.relay_id)));

  hdr("Input (controlled variable)");
  row("Source prefix",   inTxt("spfx", esc(g_cfg.src_prefix)));
  row("Category",        inTxt("scat", esc(g_cfg.src_category)));
  row("UECS type",       inTxt("styp", esc(g_cfg.input_type)));
  row("Stale timeout (s)", inTxt("sstl", String(g_cfg.src_stale_s), "number"));

  hdr("Hysteresis rule");
  row("Direction", String("<select name='dir'>")
      + "<option value=0" + (g_cfg.dir==DIR_RAISE?" selected":"") + ">RAISE (heat/dose: ON when low)</option>"
      + "<option value=1" + (g_cfg.dir==DIR_LOWER?" selected":"") + ">LOWER (vent/cool: ON when high)</option></select>");
  row("ON point",  inTxt("onp",  String(g_cfg.on_point, 2)));
  row("OFF point", inTxt("offp", String(g_cfg.off_point, 2)));

  hdr("Hard limits (force OFF)");
  row("Hard-max enable", inChk("hhie", g_cfg.hard_hi_en));
  row("Hard-max value",  inTxt("hhi", String(g_cfg.hard_hi, 2)));
  row("Hard-min enable", inChk("hloe", g_cfg.hard_lo_en));
  row("Hard-min value",  inTxt("hlo", String(g_cfg.hard_lo, 2)));

  hdr("Schedule (optional)");
  row("Enable",          inChk("sce", g_cfg.sched_enabled));
  row("Start (min from midnight)", inTxt("scs", String(g_cfg.sched_start_min), "number"));
  row("End (min from midnight)",   inTxt("sce2", String(g_cfg.sched_end_min), "number"));

  hdr("External gate (optional)");
  row("Enable",          inChk("gte", g_cfg.gate_enabled));
  row("Gate topic",      inTxt("gtt", esc(g_cfg.gate_topic)));
  row("Suppress when value >", inTxt("gta", String(g_cfg.gate_open_above, 2)));
  row("Fail-open (unknown→suppress)", inChk("gtfo", g_cfg.gate_fail_open));
  row("Gate stale (s)",  inTxt("gtstl", String(g_cfg.gate_stale_s), "number"));

  hdr("Dwell / duty / watchdog");
  row("Min ON (s)",      inTxt("mon",  String(g_cfg.min_on_s), "number"));
  row("Min OFF (s)",     inTxt("moff", String(g_cfg.min_off_s), "number"));
  row("Duty window (s, 0=off)", inTxt("dws", String(g_cfg.duty_window_s), "number"));
  row("Duty max (%)",    inTxt("dpm",  String(g_cfg.duty_pct_max), "number"));
  row("Max-on watchdog (s, 0=off)", inTxt("mxon", String(g_cfg.max_on_s), "number"));
  return s;
}

// ---- Apply the config rows (hook) ------------------------------------------
static void applyConfigSensorForm(const String &body) {
  g_cfg.mode = (uint8_t)agri::parseFormInt(body, "mode", g_cfg.mode);

  int newpin = agri::parseFormInt(body, "rpin", g_cfg.relay_pin);
  g_cfg.active_high = agri::parseFormBool(body, "rah");
  agri::parseFormStr(body, "rid", g_cfg.relay_id, sizeof(g_cfg.relay_id));

  agri::parseFormStr(body, "spfx", g_cfg.src_prefix,   sizeof(g_cfg.src_prefix));
  agri::parseFormStr(body, "scat", g_cfg.src_category, sizeof(g_cfg.src_category));
  agri::parseFormStr(body, "styp", g_cfg.input_type,   sizeof(g_cfg.input_type));
  g_cfg.src_stale_s = (uint16_t)agri::parseFormInt(body, "sstl", g_cfg.src_stale_s);

  g_cfg.dir       = (uint8_t)agri::parseFormInt(body, "dir", g_cfg.dir);
  g_cfg.on_point  = pfFloat(body, "onp",  g_cfg.on_point);
  g_cfg.off_point = pfFloat(body, "offp", g_cfg.off_point);

  g_cfg.hard_hi_en = agri::parseFormBool(body, "hhie");
  g_cfg.hard_hi    = pfFloat(body, "hhi", g_cfg.hard_hi);
  g_cfg.hard_lo_en = agri::parseFormBool(body, "hloe");
  g_cfg.hard_lo    = pfFloat(body, "hlo", g_cfg.hard_lo);

  g_cfg.sched_enabled   = agri::parseFormBool(body, "sce");
  g_cfg.sched_start_min = (uint16_t)agri::parseFormInt(body, "scs",  g_cfg.sched_start_min);
  g_cfg.sched_end_min   = (uint16_t)agri::parseFormInt(body, "sce2", g_cfg.sched_end_min);

  g_cfg.gate_enabled    = agri::parseFormBool(body, "gte");
  agri::parseFormStr(body, "gtt", g_cfg.gate_topic, sizeof(g_cfg.gate_topic));
  g_cfg.gate_open_above = pfFloat(body, "gta", g_cfg.gate_open_above);
  g_cfg.gate_fail_open  = agri::parseFormBool(body, "gtfo");
  g_cfg.gate_stale_s    = (uint16_t)agri::parseFormInt(body, "gtstl", g_cfg.gate_stale_s);

  g_cfg.min_on_s      = (uint16_t)agri::parseFormInt(body, "mon",  g_cfg.min_on_s);
  g_cfg.min_off_s     = (uint16_t)agri::parseFormInt(body, "moff", g_cfg.min_off_s);
  g_cfg.duty_window_s = (uint16_t)agri::parseFormInt(body, "dws",  g_cfg.duty_window_s);
  g_cfg.duty_pct_max  = (uint8_t) agri::parseFormInt(body, "dpm",  g_cfg.duty_pct_max);
  g_cfg.max_on_s      = (uint16_t)agri::parseFormInt(body, "mxon", g_cfg.max_on_s);

  if ((uint8_t)newpin != g_cfg.relay_pin) controlRebindPin((uint8_t)newpin);
  sourcesRetarget();                 // re-subscribe + drop cached value (fail-safe)
  agri::MQTT::mqtt.disconnect();     // reconnect picks up a changed host/prefix
}

// ---- extra /api/status fields (hook) ---------------------------------------
static void addStatusFields(JsonObject root) {
  root["relay"]   = g_relay;
  root["mode"]    = modeStr(g_cfg.mode);
  root["reason"]  = g_reason ? g_reason : "";
  root["input_type"] = g_cfg.input_type;
  if (valFresh()) root["input"] = g_val; else root["input"] = nullptr;
  root["input_src"] = g_val_src;
  if (g_cfg.duty_window_s) root["duty_pct"] = dutyPctNow();
}

// ---- MQTT state publish ----------------------------------------------------
static String stateTopic() {
  return String(g_cfg.common.mqtt_topic_prefix) + "/relay/" + g_cfg.relay_id;
}
static bool publishState() {
  if (!agri::MQTT::connected()) return false;
  JsonDocument doc;
  doc["state"]  = g_relay ? 1 : 0;
  doc["mode"]   = modeStr(g_cfg.mode);
  doc["reason"] = g_reason ? g_reason : "";
  if (valFresh()) doc["input"] = g_val;
  doc["src"]    = g_val_src;
  doc["ts"]     = (uint32_t)time(nullptr);
  char buf[256]; size_t n = serializeJson(doc, buf, sizeof(buf));
  return agri::MQTT::mqtt.publish(stateTopic().c_str(), (const uint8_t*)buf, n, true);
}

// ---- button: cycle AUTO → FORCE OFF → FORCE ON → AUTO ----------------------
// Returns true when a press changed the mode, so loop() re-evaluates control
// immediately (no up-to-1 s wait for the next tick → the relay tracks the button).
static bool buttonPoll() {
  static bool last = true;             // pulled up = released
  static uint32_t lastMs = 0;
  bool now = digitalRead(PIN_BUTTON);
  bool changed = false;
  if (last && !now && millis() - lastMs > 250) {   // falling edge, debounced
    lastMs = millis();
    g_cfg.mode = (g_cfg.mode == MODE_AUTO)      ? MODE_FORCE_OFF
               : (g_cfg.mode == MODE_FORCE_OFF) ? MODE_FORCE_ON
                                                : MODE_AUTO;
    Serial.printf("[BTN] mode -> %s\n", modeStr(g_cfg.mode));
    changed = true;
  }
  last = now;
  return changed;
}

// ---- setup -----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== %s v%s ===\n", FW_NAME, FW_VERSION);

  appled::begin();
  loadConfig();
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  controlBegin();                      // relay de-energized (fail-safe)

  Serial.printf("[CFG] node=%s prefix=%s input=%s/%s/%s pin=G%d %s\n",
                g_cfg.common.node_id, g_cfg.common.mqtt_topic_prefix,
                g_cfg.src_prefix, g_cfg.src_category, g_cfg.input_type,
                g_cfg.relay_pin, g_cfg.active_high ? "HIGH" : "LOW");

  agri::W5500Pins pins;
  pins.sck = W5500_SCK; pins.miso = W5500_MISO; pins.mosi = W5500_MOSI; pins.cs = W5500_CS;
  agri::Network::begin(g_cfg.common.hostname, pins);
  agri::Network::waitForLease();

  configTime(0, 0, "ntp.nict.jp", "pool.ntp.org");   // SNTP for schedule + ts

  agri::MQTT::begin();
  sourcesBegin();

  agri::WebHooks hooks;
  hooks.nodeTitle              = [](){ return FW_NAME; };
  hooks.renderDashboardSensors = renderDashboardSensors;
  hooks.renderConfigSensorRows = renderConfigSensorRows;
  hooks.applyConfigSensorForm  = applyConfigSensorForm;
  hooks.addStatusFields        = addStatusFields;
  hooks.saveConfig             = [](){ saveConfig(); };
  agri::WebUI::begin(g_cfg.common, hooks, FW_NAME, FW_VERSION);

  agri::mdnsBegin(g_cfg.common.hostname);
  agri::otaBegin(g_cfg.common.hostname);
  agri::OTA::begin(FW_REPO, FW_BIN_NAME, FW_VERSION);
  agri::OTA::checkLatest();

  Serial.printf("[BOOT] %s %s  mqtt=%s  ccm=%s\n", FW_NAME, FW_VERSION,
                g_cfg.common.mqtt_host[0] ? g_cfg.common.mqtt_host : "(none)",
                g_cfg.common.ccm_enabled ? "on" : "off");
}

// ---- loop ------------------------------------------------------------------
void loop() {
  static uint32_t lastMqttTryMs = 0, lastCtrlMs = 0, lastPubMs = 0;
  static bool     wasConnected = false;

  agri::otaHandle();
  agri::OTA::poll();
  agri::WebUI::handle(agri::Network::link_up, agri::Network::have_lease);
  agri::ProvisionAP::poll(agri::Network::have_lease, g_cfg.common.hostname);
  bool btnChanged = buttonPoll();

  uint32_t now = millis();

  // MQTT keepalive / reconnect; (re)subscribe on the connect edge.
  if (agri::networkUp() && agri::MQTT::hasHost(g_cfg.common)) {
    if (!agri::MQTT::connected()) {
      if (now - lastMqttTryMs > 5000) { lastMqttTryMs = now; agri::MQTT::reconnect(g_cfg.common); }
    } else {
      if (!wasConnected) { sourcesSubscribe(); publishState(); }
      agri::MQTT::loop();
    }
  }
  wasConnected = agri::MQTT::connected();

  if (g_cfg.common.ccm_enabled) sourcesCcmPoll();

  // Control tick ~1 Hz, or immediately after a button press so the relay tracks
  // the operator without the up-to-1 s tick latency.
  if (btnChanged || now - lastCtrlMs >= 1000) {
    lastCtrlMs = now;
    bool prev = g_relay;
    controlTick(now);
    bool changed = (g_relay != prev);
    if (changed ||
        (agri::MQTT::connected() && now - lastPubMs >= (uint32_t)g_cfg.common.mqtt_interval_s * 1000UL)) {
      lastPubMs = now;
      if (publishState()) appled::flashPublish();
    }
  }

  // Status LED
  appled::State led;
  if      (!agri::networkUp())                                            led = appled::NO_LINK;
  else if (agri::MQTT::hasHost(g_cfg.common) && !agri::MQTT::connected())  led = appled::NO_MQTT;
  else if (g_relay)                                                       led = appled::ON;
  else if (g_cfg.mode == MODE_AUTO && !valFresh())                        led = appled::STALE;
  else                                                                    led = appled::OK;
  appled::set(led);

  delay(20);
}
