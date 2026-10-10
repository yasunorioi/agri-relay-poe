# agri-relay-poe

🇯🇵 [日本語](README_ja.md) ・ **🇬🇧 English**

An **autonomous relay node** for the agri-* family, built on an M5Stack **AtomS3 Lite** + **Atomic PoE Base (K139 / W5500)** + **Unit Relay (U002, 1-ch SPDT)**.

It watches **one input value** (an agriha MQTT topic, with optional UECS-CCM fallback) and drives the Unit Relay through a **declarative safety envelope** — hysteresis, time-of-day schedule, external gate, min on/off dwell, duty limit, max-on watchdog, and fail-safe OFF on stale input. It generalizes [agri-co2-burner](https://github.com/yasunorioi/agri-co2-burner) (CO2-specific, WiFi) into a **PoE node whose controlled variable and setpoints are configurable**, so it is not tied to one load.

> **Design**: autonomous (keeps running if the Pi4/yasu-hp master dies), but the node stays a thin **decision + I/O** unit — the rule is a single hysteresis primitive, not a hierarchical controller. Remote override via an MQTT command topic; local via the button.

---

## Hardware

| Item | Value |
|---|---|
| MCU | M5Stack AtomS3 Lite (ESP32-S3) |
| Net / power | Atomic PoE Base (K139, W5500, 802.3at PoE PD). One PoE cable for power + data |
| Relay | M5Stack **Unit Relay (U002)**: 1-ch **SPDT** (HK4100F), **AC250V/3A or DC30V/3A**, 5V coil 40mA |
| Connection | Grove HY2.0-4P: **Yellow=DIN (control, active HIGH) / Red=5V / Black=GND / White=NC contact**. Plug into the AtomS3 Grove port |
| Control pin | **DIN = G2** (Grove yellow/SIG) by default. 3.3V logic HIGH triggers the on-unit driver; the coil is powered from Grove 5V. Editable in `/config` |
| LED | onboard WS2812C = **G35** (not the core's G27) |
| Button | onboard = **G41** (cycles AUTO / FORCE OFF / FORCE ON) |
| W5500 SPI | **SCK=5 / CS=6 / MISO=7 / MOSI=8** (AtomS3 + K139; not the classic ATOM's G22/23/33/19) |

## Control model — generic single-variable hysteresis thermostat

The node watches an input value `v` (`<src_prefix>/<src_category>/<input_type>` over agriha MQTT, optional CCM) and switches the relay by a **direction**-aware hysteresis rule:

| Direction | Use case | ON | OFF | Setpoints |
|---|---|---|---|---|
| **RAISE** | heater / CO2 / humidifier | `v ≤ on_point` | `v ≥ off_point` | on_point < off_point |
| **LOWER** | vent / cooler / dehumidifier | `v ≥ on_point` | `v ≤ off_point` | on_point > off_point |

**Decision priority** (highest first, evaluated ~1 Hz):
1. **S1 hard OFF**: FORCE OFF / input stale (AUTO only, fail-safe) / hard hi-lo limit (whenever `v` is fresh, even under FORCE ON) / max-on watchdog → OFF now
2. **S2 duty budget exhausted** → OFF
3. **S3/S4 dwell** (AUTO only): min-off before re-energizing / min-on hold — a manual FORCE ON/OFF switches immediately
4. **N normal**: FORCE ON, or hysteresis AND schedule AND gate

S1/S2 override the min-on hold — when in doubt, OFF. The relay is de-energized at boot. A persisted FORCE ON is downgraded to AUTO on boot (so an unattended reboot never energizes a combustion load).

**Mode**: AUTO / FORCE ON / FORCE OFF — set by the button (G41, cycles), the WebUI Config *Mode* select, or an MQTT command.

## MQTT

- **Input (subscribe)**: `<src_prefix>/<src_category>/<input_type>` (e.g. `agriha/3/sensor/InAirCO2`), agriha `{value,…}` payloads. `src_category` = `sensor` for a house with a native publisher, `sensor_ccm` for one bridged via CCM.
- **CCM fallback** (when `ccm_enabled`): listens on UDP 16520 for `<DATA type="<input_type>…">`.
- **External gate** (optional): subscribes another topic's `{value}`/`{pct}` and suppresses ON when `value > gate_open_above` (e.g. a side window open).
- **State (publish, retained)**: `<prefix>/relay/<relay_id>` → `{state:0/1, mode, reason, input, src, ts}`, on every relay transition and every `mqtt_interval_s`.
- **Remote command**: `<prefix>/relay/<relay_id>/cmd` → `{"mode":"auto|on|off"}` or `{"on":true/false}`.
- **LWT**: core default `<prefix>/sys/<node_id>/online`.

## Build / flash

```
pio run                       # build (default = house3: agriha/3 / sensor)
pio run -t upload             # first flash over USB-C (native USB CDC)

# house2 node (CO2 bridged via CCM = agriha/2 / sensor_ccm, node=relay_poe_02):
PLATFORMIO_BUILD_FLAGS="-DHOUSE2" pio run -t upload
```
> ⚠ When you change the compiled defaults (setDefaults), **stale NVS overrides them**, so erase first:
> `python -m esptool --port <port> erase_flash`, then `pio run -t upload`. Final per-node config is set on the greenhouse LAN via the WebUI `/config` (persisted to NVS).

- platform = pioarduino fork `#55.03.38` (arduino-esp32 3.x), board = `m5stack-atoms3`, core = `agri-node-poe-core@v0.7.0`.
- Later flashes over Ethernet via ArduinoOTA (`agri-relay-poe-01.local`) / WebUI `POST /api/ota` / GitHub-release self-update.

### Release / self-update (OTA)
AgriOTA polls `releases/latest` at boot and daily, and flashes `agri-relay-poe.bin` **only on a user click** (WebUI Update button / `POST /api/update`). To cut a new version:
```
# bump FW_VERSION in src/main.cpp (e.g. 0.1.1) → commit
pio run
cp .pio/build/m5atoms3-poe/firmware.bin agri-relay-poe.bin
gh release create v0.1.1 --target main --title v0.1.1 --notes "…" agri-relay-poe.bin
```
- Tag must be `vX.Y.Z`; the asset must be named exactly `agri-relay-poe.bin` (otherwise the tag is found but the download 404s).
- The OTA image is a single generic build (default = house3). **Each node's house2/3 identity lives in NVS** — save `/config` once on the greenhouse LAN and OTA preserves it (OTA before saving reverts to the house3 default).
- The OTA check needs reachability to api.github.com (the site's internet link).

## Config (WebUI `http://agri-relay-poe-01.local/` or the DHCP IP)

Config page: common (MQTT / CCM envelope) + Relay (mode, DIN pin, active HIGH, relay_id) + Input (prefix/category/type, stale) + Hysteresis (direction, ON/OFF points) + Hard limits (hi/lo) + Schedule + External gate + dwell/duty/watchdog. All persisted to NVS (`relayp-cfg`). The Dashboard shows an ON/OFF banner, mode, reason, input value, and the active rule.

## Defaults (ships as a CO2-burner dosing profile)

For the first deployment the defaults mirror the proven agri-co2-burner parameters (driving a Shizuoka Seiki CG-1000 via the Unit Relay). Fully re-targetable in `/config` — this is a generic relay node that ships with a CO2 profile.

| Field | Default | Meaning |
|---|---|---|
| Input | `agriha/3/sensor/InAirCO2` | house3 CO2 (native = `sensor`). house2 CO2 is CCM-bridged → `sensor_ccm` (build `-DHOUSE2`) |
| Direction | **RAISE** | relay raises CO2 = ON when CO2 ≤ on_point |
| ON / OFF | 450 / 520 ppm | dose below 450, stop at/above 520 |
| Hard limit | **≥1000 ppm → OFF** (always) | CG-1000 spec; wins even under FORCE ON when `v` is fresh |
| Schedule | 08:00–15:00 | daytime only (photosynthesis); adjust to the house |
| Dwell | min on/off = 300 s | anti short-cycle (AUTO only) |
| Duty | 50% / 3600 s window | CG-1000 duty ≤ 50%/h |
| Stale | 180 s | CO2 stale → fail-safe OFF |
| State topic | `agriha/3/relay/co2` | retained |
| Mode | AUTO | a persisted FORCE ON is downgraded to AUTO on boot (never fire a burner on an unattended reboot) |
| Window gate | OFF | agri-co2-burner suppresses dosing while a side window is open (polls ArSprout); this node gates on an MQTT `{value}/{pct}` topic instead (set `gate_topic` to enable) |

⚠ The existing WiFi **agri-co2-burner** also controls the CG-1000. Only **one** controller (this PoE node *or* the WiFi one) should drive a given burner — never both.

For other loads (heater / vent / irrigation …), just change input / direction / ON-OFF points / limits / duty in `/config`.

## Related

- Shared library: [agri-node-poe-core](https://github.com/yasunorioi/agri-node-poe-core)
- CO2-specific autonomous relay (origin): [agri-co2-burner](https://github.com/yasunorioi/agri-co2-burner)
- Sibling PoE nodes: agri-env-poe / agri-temp-poe / agri-rain-poe / agri-flow-poe / agri-solar-poe / agri-uv-poe
- Unrelated to `agri-relay` (an RP2350-based OGMS relay node).
