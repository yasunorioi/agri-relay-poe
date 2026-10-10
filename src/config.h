// config.h — agri-relay-poe NVS-backed config ("relayp-cfg").
//
// AtomS3 Lite + Atomic PoE Base (W5500) + M5 Unit Relay. Network / MQTT /
// UECS-CCM envelope come from agri-node-poe-core's CommonConfig; this struct
// adds the relay pin + the single-variable hysteresis rule and its safety
// envelope. Every field is editable in /config.
//
// CONTROL MODEL (generic, generalized from agri-co2-burner)
// ---------------------------------------------------------
// The node watches ONE input value v (an agriha MQTT topic, optional CCM
// fallback) and drives the relay by a hysteresis rule with a direction:
//   DIR_RAISE : relay raises v (heater / CO2 / humidifier)
//               ON when v <= on_point, OFF when v >= off_point  (on_point < off_point)
//   DIR_LOWER : relay lowers v (vent / cooler / dehumidifier)
//               ON when v >= on_point, OFF when v <= off_point  (on_point > off_point)
// bounded by: schedule window, external gate, min on/off dwell, duty limit,
// max-on watchdog, hard hi/lo limits, and fail-safe OFF when v is stale.

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <AgriCommonConfig.h>

enum RelayDir  { DIR_RAISE = 0, DIR_LOWER = 1 };
enum RelayMode { MODE_AUTO = 0, MODE_FORCE_ON = 1, MODE_FORCE_OFF = 2 };

struct AppConfig {
  agri::CommonConfig common;

  // ---- relay hardware ----
  uint8_t relay_pin;        // DIN GPIO (default G2 = AtomS3 Grove yellow/SIG)
  bool    active_high;      // true: HIGH energizes the Unit Relay (default)
  char    relay_id[12];     // id in the state/cmd topic (default "1")

  // ---- input source (the controlled variable) ----
  char     src_prefix[48];  // e.g. "agriha/2"
  char     src_category[16];// "sensor" (native) or "sensor_ccm" (CCM bridge)
  char     input_type[24];  // UECS type, e.g. "InAirTemp" (topic = prefix/cat/type)
  uint16_t src_stale_s;     // input older than this -> fail-safe OFF

  // ---- hysteresis rule ----
  uint8_t dir;              // RelayDir
  float   on_point;         // see DIR_* above
  float   off_point;

  // ---- hard limits (safety override -> OFF). *_en gates each ----
  bool  hard_hi_en;  float hard_hi;   // v >= hard_hi -> OFF
  bool  hard_lo_en;  float hard_lo;   // v <= hard_lo -> OFF

  // ---- schedule (optional time-of-day window) ----
  bool     sched_enabled;
  uint16_t sched_start_min; // minutes from midnight (local)
  uint16_t sched_end_min;   // wraps past midnight if start > end

  // ---- external gate (optional): suppress ON when a gate value is "open" ----
  bool     gate_enabled;
  char     gate_topic[64];  // full MQTT topic carrying a numeric {value}
  float    gate_open_above; // gate value > this => suppress (e.g. window % open)
  bool     gate_fail_open;  // gate stale/unknown => treat as open (suppress)?
  uint16_t gate_stale_s;

  // ---- dwell / duty / watchdog ----
  uint16_t min_on_s;        // minimum ON time once energized
  uint16_t min_off_s;       // minimum OFF time before re-energizing
  uint16_t duty_window_s;   // tumbling duty window (0 = no duty limit)
  uint8_t  duty_pct_max;    // max % ON within the duty window
  uint16_t max_on_s;        // hard OFF after this much continuous ON (0 = off)

  // ---- mode ----
  uint8_t mode;             // RelayMode (persisted; also set by button / MQTT cmd)
};

extern AppConfig g_cfg;

inline void setDefaults() {
  agri::commonDefaults(g_cfg.common,
                       "relay_poe_01", "agri-relay-poe-01",
                       /*mqtt prefix = house scope*/ "agriha/2",
                       /*default_ccm_region = 別棟 ArSprout region*/ 13);

  g_cfg.relay_pin   = 2;            // AtomS3 Grove yellow/SIG = Unit Relay DIN
  g_cfg.active_high = true;
  strlcpy(g_cfg.relay_id, "1", sizeof(g_cfg.relay_id));

  strlcpy(g_cfg.src_prefix,   "agriha/2",    sizeof(g_cfg.src_prefix));
  strlcpy(g_cfg.src_category, "sensor",      sizeof(g_cfg.src_category));
  strlcpy(g_cfg.input_type,   "InAirTemp",   sizeof(g_cfg.input_type));
  g_cfg.src_stale_s = 120;

  g_cfg.dir       = DIR_LOWER;      // default: vent/cool example (ON when hot)
  g_cfg.on_point  = 30.0f;          // ON  at v >= 30
  g_cfg.off_point = 28.0f;          // OFF at v <= 28

  g_cfg.hard_hi_en = false; g_cfg.hard_hi = 0.0f;
  g_cfg.hard_lo_en = false; g_cfg.hard_lo = 0.0f;

  g_cfg.sched_enabled   = false;
  g_cfg.sched_start_min = 0;
  g_cfg.sched_end_min   = 0;

  g_cfg.gate_enabled    = false;
  g_cfg.gate_topic[0]   = '\0';
  g_cfg.gate_open_above = 0.0f;
  g_cfg.gate_fail_open  = true;     // unknown gate => assume open (safe suppress)
  g_cfg.gate_stale_s    = 300;

  g_cfg.min_on_s      = 30;
  g_cfg.min_off_s     = 30;
  g_cfg.duty_window_s = 0;          // no duty limit by default
  g_cfg.duty_pct_max  = 100;
  g_cfg.max_on_s      = 0;          // watchdog off by default

  g_cfg.mode = MODE_AUTO;
}

inline void loadConfig() {
  setDefaults();
  Preferences p;
  if (!p.begin("relayp-cfg", true)) return;
  agri::commonLoad(g_cfg.common, p);

  g_cfg.relay_pin   = p.getUChar("rpin", g_cfg.relay_pin);
  g_cfg.active_high = p.getBool ("rah",  g_cfg.active_high);
  { String s = p.getString("rid", g_cfg.relay_id); strlcpy(g_cfg.relay_id, s.c_str(), sizeof(g_cfg.relay_id)); }

  { String s = p.getString("spfx", g_cfg.src_prefix);   strlcpy(g_cfg.src_prefix,   s.c_str(), sizeof(g_cfg.src_prefix)); }
  { String s = p.getString("scat", g_cfg.src_category); strlcpy(g_cfg.src_category, s.c_str(), sizeof(g_cfg.src_category)); }
  { String s = p.getString("styp", g_cfg.input_type);   strlcpy(g_cfg.input_type,   s.c_str(), sizeof(g_cfg.input_type)); }
  g_cfg.src_stale_s = p.getUShort("sstl", g_cfg.src_stale_s);

  g_cfg.dir       = p.getUChar("dir",  g_cfg.dir);
  g_cfg.on_point  = p.getFloat("onp",  g_cfg.on_point);
  g_cfg.off_point = p.getFloat("offp", g_cfg.off_point);

  g_cfg.hard_hi_en = p.getBool ("hhie", g_cfg.hard_hi_en);
  g_cfg.hard_hi    = p.getFloat("hhi",  g_cfg.hard_hi);
  g_cfg.hard_lo_en = p.getBool ("hloe", g_cfg.hard_lo_en);
  g_cfg.hard_lo    = p.getFloat("hlo",  g_cfg.hard_lo);

  g_cfg.sched_enabled   = p.getBool  ("sce",  g_cfg.sched_enabled);
  g_cfg.sched_start_min = p.getUShort("scs",  g_cfg.sched_start_min);
  g_cfg.sched_end_min   = p.getUShort("sce2", g_cfg.sched_end_min);

  g_cfg.gate_enabled    = p.getBool ("gte",  g_cfg.gate_enabled);
  { String s = p.getString("gtt", g_cfg.gate_topic); strlcpy(g_cfg.gate_topic, s.c_str(), sizeof(g_cfg.gate_topic)); }
  g_cfg.gate_open_above = p.getFloat ("gta",  g_cfg.gate_open_above);
  g_cfg.gate_fail_open  = p.getBool  ("gtfo", g_cfg.gate_fail_open);
  g_cfg.gate_stale_s    = p.getUShort("gtstl", g_cfg.gate_stale_s);

  g_cfg.min_on_s      = p.getUShort("mon",  g_cfg.min_on_s);
  g_cfg.min_off_s     = p.getUShort("moff", g_cfg.min_off_s);
  g_cfg.duty_window_s = p.getUShort("dws",  g_cfg.duty_window_s);
  g_cfg.duty_pct_max  = p.getUChar ("dpm",  g_cfg.duty_pct_max);
  g_cfg.max_on_s      = p.getUShort("mxon", g_cfg.max_on_s);

  g_cfg.mode = p.getUChar("mode", g_cfg.mode);
  p.end();
}

inline bool saveConfig() {
  Preferences p;
  if (!p.begin("relayp-cfg", false)) return false;
  agri::commonSave(g_cfg.common, p);

  p.putUChar ("rpin", g_cfg.relay_pin);
  p.putBool  ("rah",  g_cfg.active_high);
  p.putString("rid",  g_cfg.relay_id);

  p.putString("spfx", g_cfg.src_prefix);
  p.putString("scat", g_cfg.src_category);
  p.putString("styp", g_cfg.input_type);
  p.putUShort("sstl", g_cfg.src_stale_s);

  p.putUChar ("dir",  g_cfg.dir);
  p.putFloat ("onp",  g_cfg.on_point);
  p.putFloat ("offp", g_cfg.off_point);

  p.putBool  ("hhie", g_cfg.hard_hi_en);
  p.putFloat ("hhi",  g_cfg.hard_hi);
  p.putBool  ("hloe", g_cfg.hard_lo_en);
  p.putFloat ("hlo",  g_cfg.hard_lo);

  p.putBool  ("sce",  g_cfg.sched_enabled);
  p.putUShort("scs",  g_cfg.sched_start_min);
  p.putUShort("sce2", g_cfg.sched_end_min);

  p.putBool  ("gte",  g_cfg.gate_enabled);
  p.putString("gtt",  g_cfg.gate_topic);
  p.putFloat ("gta",  g_cfg.gate_open_above);
  p.putBool  ("gtfo", g_cfg.gate_fail_open);
  p.putUShort("gtstl", g_cfg.gate_stale_s);

  p.putUShort("mon",  g_cfg.min_on_s);
  p.putUShort("moff", g_cfg.min_off_s);
  p.putUShort("dws",  g_cfg.duty_window_s);
  p.putUChar ("dpm",  g_cfg.duty_pct_max);
  p.putUShort("mxon", g_cfg.max_on_s);

  p.putUChar ("mode", g_cfg.mode);
  p.end();
  return true;
}
