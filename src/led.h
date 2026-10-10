// led.h — single-pixel status LED for the AtomS3 Lite.
//
// agri-node-poe-core ships AgriLED.h, but it hardcodes the WS2812 data pin to
// G27 (classic ATOM Lite); the AtomS3 Lite's RGB LED is a WS2812C on G35, which
// FastLED needs as a compile-time template arg. So this node drives its own
// pixel here instead of agri::Led (same approach as agri-temp-poe).
//
//   BOOT     blue     booting / pre-network
//   NO_LINK  red      Ethernet link down or no DHCP lease
//   NO_MQTT  amber    MQTT host configured but not connected
//   STALE    magenta  input value stale -> control is in fail-safe OFF
//   OK       green    link + MQTT + fresh input, relay OFF
//   ON       orange   relay energized
//   PUB      white    momentary flash on each successful state publish

#pragma once

#include <Arduino.h>
#include <FastLED.h>

namespace appled {

static const uint8_t LED_PIN = 35;   // AtomS3 Lite on-board WS2812C

enum State { BOOT, NO_LINK, NO_MQTT, STALE, OK, ON, PUB };

inline CRGB  &pixel() { static CRGB px[1]; return px[0]; }
inline State &state() { static State s = BOOT; return s; }

inline CRGB colorFor(State s) {
  switch (s) {
    case BOOT:    return CRGB(0, 0, 50);
    case NO_LINK: return CRGB(80, 0, 0);
    case NO_MQTT: return CRGB(60, 40, 0);
    case STALE:   return CRGB(60, 0, 60);
    case OK:      return CRGB(0, 30, 0);
    case ON:      return CRGB(90, 35, 0);
    case PUB:     return CRGB(60, 60, 60);
  }
  return CRGB::Black;
}

inline void apply() { pixel() = colorFor(state()); FastLED.show(); }

inline void begin(uint8_t brightness = 30) {
  FastLED.addLeds<WS2812, LED_PIN, GRB>(&pixel(), 1);
  FastLED.setBrightness(brightness);
  state() = BOOT;
  apply();
}

inline void set(State s) {
  if (s != state()) { state() = s; apply(); }
}

inline void flashPublish() {
  State prev = state();
  state() = PUB; apply();
  delay(40);
  state() = prev; apply();
}

} // namespace appled
