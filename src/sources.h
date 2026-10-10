// sources.h — input acquisition + remote command.
//
// The controlled variable comes from ONE of:
//   1. MQTT  (primary)  — subscribe <src_prefix>/<src_category>/<input_type>,
//                         agriha {value,...} payloads. category = "sensor" for a
//                         house with a native publisher, "sensor_ccm" via the bridge.
//   2. CCM   (fallback) — UDP 16520 <DATA type="<input_type>...">, broadcast by
//                         ArSprout / agri-*-poe with CCM on. Used when ccm_enabled.
// Plus an optional external GATE value (another MQTT topic, e.g. window % open)
// and a remote COMMAND topic (<prefix>/relay/<id>/cmd) that overrides the mode.
//
// Each value carries a freshness stamp so control can fail safe when it goes stale.

#pragma once

#include <Arduino.h>
#include <NetworkUdp.h>
#include <ArduinoJson.h>
#include <AgriMQTT.h>
#include "config.h"

// ---- live values (updated by callback / CCM poll, read by control) ----------
extern float       g_val;      // input value, NAN until first reading
extern uint32_t    g_val_ms;   // millis() of last input update (0 = never)
extern const char *g_val_src;  // "mqtt" / "ccm" / "-"
extern float       g_gate;     // gate value, NAN until first reading
extern uint32_t    g_gate_ms;

// ---- topic strings built at begin()/retarget --------------------------------
inline String &inputTopic() { static String s; return s; }
inline String &cmdTopic()   { static String s; return s; }

inline NetworkUDP &ccmRxSocket() { static NetworkUDP u; return u; }

// Content of the first <DATA type="<prefix>..."> element (prefix match, so
// "InAirTemp" also hits "InAirTemp.cMC" with any room/region).
inline bool ccmExtract(const String &xml, const char *typePrefix, float &out) {
  String needle = String("type=\"") + typePrefix;
  int p = xml.indexOf(needle);  if (p < 0) return false;
  int gt = xml.indexOf('>', p); if (gt < 0) return false;
  int lt = xml.indexOf('<', gt + 1); if (lt < 0) return false;
  String v = xml.substring(gt + 1, lt); v.trim();
  if (!v.length()) return false;
  out = v.toFloat();
  return true;
}

inline void sourcesCcmPoll() {
  NetworkUDP &u = ccmRxSocket();
  int sz = u.parsePacket();
  if (sz <= 0) return;
  char buf[600];
  int n = u.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = 0;
  float v;
  if (ccmExtract(String(buf), g_cfg.input_type, v)) {
    g_val = v; g_val_ms = millis(); g_val_src = "ccm";
  }
}

// ---- remote command: {"mode":"auto|on|off"} or {"on":true/false} ------------
inline void applyCommand(const JsonDocument &doc) {
  const char *m = doc["mode"] | "";
  if (!strcmp(m, "auto")) g_cfg.mode = MODE_AUTO;
  else if (!strcmp(m, "on"))  g_cfg.mode = MODE_FORCE_ON;
  else if (!strcmp(m, "off")) g_cfg.mode = MODE_FORCE_OFF;
  else if (!doc["on"].isNull()) g_cfg.mode = doc["on"].as<bool>() ? MODE_FORCE_ON : MODE_FORCE_OFF;
  else return;
  Serial.printf("[CMD] mode <- %d\n", g_cfg.mode);
}

// ---- single MQTT callback: input / gate / command ---------------------------
inline void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;

  if (cmdTopic() == topic) { applyCommand(doc); return; }

  float value = doc["value"] | NAN;
  if (isnan(value)) return;
  if (inputTopic() == topic) {
    g_val = value; g_val_ms = millis(); g_val_src = "mqtt";
  } else if (g_cfg.gate_enabled && inputTopic() != topic &&
             String(g_cfg.gate_topic) == topic) {
    g_gate = value; g_gate_ms = millis();
  }
}

inline void sourcesBuildTopics() {
  const char *cat = g_cfg.src_category[0] ? g_cfg.src_category : "sensor";
  inputTopic() = String(g_cfg.src_prefix) + "/" + cat + "/" + g_cfg.input_type;
  cmdTopic()   = String(g_cfg.common.mqtt_topic_prefix) + "/relay/" + g_cfg.relay_id + "/cmd";
}

inline void sourcesSubscribe() {
  agri::MQTT::mqtt.subscribe(inputTopic().c_str());
  agri::MQTT::mqtt.subscribe(cmdTopic().c_str());
  if (g_cfg.gate_enabled && g_cfg.gate_topic[0]) agri::MQTT::mqtt.subscribe(g_cfg.gate_topic);
  Serial.printf("[SUB] in=%s cmd=%s gate=%s\n",
                inputTopic().c_str(), cmdTopic().c_str(),
                g_cfg.gate_enabled ? g_cfg.gate_topic : "(off)");
}

inline void sourcesBegin() {
  sourcesBuildTopics();
  agri::MQTT::mqtt.setCallback(onMqttMessage);
  if (g_cfg.common.ccm_enabled) ccmRxSocket().begin(16520);
}

// Re-point subscriptions after the source fields change on /config. Drops the
// cached value so a stale reading from the old source cannot linger as "fresh" —
// control fails safe until the new topic delivers.
inline void sourcesRetarget() {
  String oldIn = inputTopic(), oldCmd = cmdTopic();
  sourcesBuildTopics();
  if (agri::MQTT::mqtt.connected()) {
    if (oldIn.length())  agri::MQTT::mqtt.unsubscribe(oldIn.c_str());
    if (oldCmd.length() && oldCmd != cmdTopic()) agri::MQTT::mqtt.unsubscribe(oldCmd.c_str());
    sourcesSubscribe();
  }
  g_val = NAN; g_val_ms = 0; g_val_src = "-";
  g_gate = NAN; g_gate_ms = 0;
}

// ---- freshness --------------------------------------------------------------
inline bool valFresh() {
  return g_val_ms != 0 && !isnan(g_val) &&
         (millis() - g_val_ms) < (uint32_t)g_cfg.src_stale_s * 1000UL;
}
inline bool gateFresh() {
  return g_gate_ms != 0 && !isnan(g_gate) &&
         (millis() - g_gate_ms) < (uint32_t)g_cfg.gate_stale_s * 1000UL;
}
