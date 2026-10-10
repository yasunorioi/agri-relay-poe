// control.h — autonomous single-variable hysteresis decision + relay drive.
//
// Decision priority (highest first), evaluated every controlTick():
//   S1  FORCE_OFF, input stale (fail-safe), hard hi/lo limit, or max-on watchdog → OFF
//   S2  duty budget exhausted                                                    → OFF
//   S3  min-off dwell not elapsed (want ON)                                      → stay OFF
//   S4  min-on dwell not elapsed  (want OFF, currently ON)                       → hold ON
//   N   normal: FORCE_ON, or hysteresis(dir) AND schedule AND gate
//
// S1/S2 are safety/spec limits and override the S4 min-on hold — when in doubt, OFF.

#pragma once

#include <Arduino.h>
#include <time.h>
#include "config.h"
#include "sources.h"

// ---- live control state (defined in main.cpp) ------------------------------
extern bool        g_relay;        // current relay state (true = energized)
extern uint32_t    g_relay_ms;     // millis() of last relay transition
extern const char *g_reason;       // human-readable last decision reason

extern uint32_t    g_duty_win_start_ms;
extern uint32_t    g_duty_on_ms;   // accumulated ON time in current window
extern uint32_t    g_duty_last_ms;

inline void relayDrive(bool energize) {
  // active_high: HIGH energizes. active_low inverts (some relay boards trigger LOW).
  digitalWrite(g_cfg.relay_pin, (energize == g_cfg.active_high) ? HIGH : LOW);
}

inline void relayWrite(bool on) {
  if (on == g_relay) return;
  g_relay = on;
  g_relay_ms = millis();
  relayDrive(on);
  Serial.printf("[RELAY] %s (%s)\n", on ? "ON" : "OFF", g_reason ? g_reason : "");
}

inline void controlBegin() {
  pinMode(g_cfg.relay_pin, OUTPUT);
  g_relay = false;
  relayDrive(false);               // boot = de-energized (fail-safe)
  g_relay_ms = millis();
  g_duty_win_start_ms = millis();
  g_duty_on_ms = 0;
  g_duty_last_ms = millis();
}

// Re-init the output pin if /config changed it (de-energize the old pin first).
inline void controlRebindPin(uint8_t newPin) {
  if (newPin == g_cfg.relay_pin) { relayDrive(g_relay); return; }
  relayDrive(false);               // release the old pin
  g_cfg.relay_pin = newPin;
  pinMode(g_cfg.relay_pin, OUTPUT);
  relayDrive(g_relay);
}

inline int localMinuteOfDay() {
  time_t t = time(nullptr);
  if (t < 100000) return -1;        // SNTP not synced yet
  struct tm tmv; localtime_r(&t, &tmv);
  return tmv.tm_hour * 60 + tmv.tm_min;
}

inline bool withinSchedule() {
  if (!g_cfg.sched_enabled) return true;
  int m = localMinuteOfDay();
  if (m < 0) return false;          // no clock → outside (safe)
  int a = g_cfg.sched_start_min, b = g_cfg.sched_end_min;
  if (a == b) return false;
  if (a < b)  return m >= a && m < b;
  return m >= a || m < b;           // overnight wrap
}

// Allowed to run w.r.t. the external gate? (e.g. suppress while a window is open)
inline bool gateOk() {
  if (!g_cfg.gate_enabled) return true;
  if (!gateFresh())        return !g_cfg.gate_fail_open;  // unknown → fail_open suppresses
  return g_gate <= g_cfg.gate_open_above;
}

inline uint8_t dutyPctNow() {
  if (g_cfg.duty_window_s == 0) return 0;
  return (uint8_t)((uint64_t)g_duty_on_ms * 100ULL /
                   ((uint32_t)g_cfg.duty_window_s * 1000ULL));
}
inline bool dutyExhausted() {
  if (g_cfg.duty_window_s == 0) return false;
  return dutyPctNow() >= g_cfg.duty_pct_max;
}
inline void dutyIntegrate(uint32_t now) {
  if (g_cfg.duty_window_s &&
      now - g_duty_win_start_ms >= (uint32_t)g_cfg.duty_window_s * 1000UL) {
    g_duty_win_start_ms = now;
    g_duty_on_ms = 0;
  }
  if (g_relay) g_duty_on_ms += (now - g_duty_last_ms);
  g_duty_last_ms = now;
}

// Hysteresis demand for the configured direction.
inline bool hysteresisCalls() {
  if (g_cfg.dir == DIR_RAISE)
    return g_relay ? (g_val <  g_cfg.off_point) : (g_val <= g_cfg.on_point);
  else // DIR_LOWER
    return g_relay ? (g_val >  g_cfg.off_point) : (g_val >= g_cfg.on_point);
}

inline void controlTick(uint32_t now) {
  dutyIntegrate(now);

  bool min_off_ok = (now - g_relay_ms) >= (uint32_t)g_cfg.min_off_s * 1000UL;
  bool min_on_ok  = (now - g_relay_ms) >= (uint32_t)g_cfg.min_on_s  * 1000UL;

  // --- S1: hard safety OFF (overrides everything, incl. min-on hold) ---------
  // FORCE OFF and the max-on watchdog are universal. HARD LIMITS apply whenever
  // the input is fresh — even under FORCE ON — so forcing on can never push a
  // combustion/CO2 load past its hard cap (CG-1000: CO2 < 1000 ppm). Only the
  // input-STALE fail-safe is AUTO-only, so a manual FORCE ON still energizes a
  // generic load / bench with no sensor present. FORCE ON stays bounded by the
  // watchdog, hard limits, duty, and dwell.
  if (g_cfg.mode == MODE_FORCE_OFF) { g_reason = "manual FORCE OFF"; relayWrite(false); return; }
  if (g_relay && g_cfg.max_on_s &&
      (now - g_relay_ms) >= (uint32_t)g_cfg.max_on_s * 1000UL) {
    g_reason = "max-on watchdog → OFF"; relayWrite(false); return;
  }
  if (valFresh()) {
    if (g_cfg.hard_hi_en && g_val >= g_cfg.hard_hi) { g_reason = "hard-max limit → OFF"; relayWrite(false); return; }
    if (g_cfg.hard_lo_en && g_val <= g_cfg.hard_lo) { g_reason = "hard-min limit → OFF"; relayWrite(false); return; }
  } else if (g_cfg.mode == MODE_AUTO) {
    g_reason = "input stale → fail-safe OFF"; relayWrite(false); return;
  }

  // --- S2: duty budget -------------------------------------------------------
  if (dutyExhausted()) { g_reason = "duty budget exhausted"; relayWrite(false); return; }

  // --- demand ---------------------------------------------------------------
  bool demand;
  if (g_cfg.mode == MODE_FORCE_ON) {
    demand = true;                          // still bounded by S1/S2 above
    g_reason = "manual FORCE ON";
  } else {
    bool calls = hysteresisCalls();
    bool sched = withinSchedule();
    bool gate  = gateOk();
    demand = calls && sched && gate;
    if      (!calls) g_reason = g_relay ? "setpoint reached → release" : "within deadband";
    else if (!sched) g_reason = "outside schedule";
    else if (!gate)  g_reason = "gate suppressed (e.g. window open)";
    else             g_reason = "demand → energize";
  }

  // --- dwell ----------------------------------------------------------------
  if (demand) {
    if (!g_relay && !min_off_ok) { g_reason = "min-off dwell"; return; }
    relayWrite(true);
  } else {
    if (g_relay && !min_on_ok) { g_reason = "min-on hold"; return; }
    relayWrite(false);
  }
}
