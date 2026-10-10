# agri-relay-poe

**🇯🇵 日本語** ・ 🇬🇧 English (TODO)

M5Stack **AtomS3 Lite** + **Atomic PoE Base（K139 / W5500）** + **Unit Relay（U002, 1ch SPDT）** による、agri-* ファミリーの**自律リレーノード**。

ひとつの入力値（agriha MQTT のトピック、任意で UECS-CCM フォールバック）を監視し、**宣言的な安全エンベロープ**（ヒステリシス / 時間窓 / 外部ゲート / min on-off dwell / duty 制限 / max-on watchdog / 入力 stale 時フェイルセーフ OFF）でリレーを駆動する。[agri-co2-burner](https://github.com/yasunorioi/agri-co2-burner)（CO2 専用・WiFi）を、**制御対象と設定値を可変にした PoE 汎用ノード**へ一般化したもの。

> **設計方針**: 自律（Pi4/yasu-hp 母艦が死んでも動き続ける）だが、ノードは**薄い「判断＋I/O」ユニット**に留める — ルールは単一のヒステリシス primitive であって、階層制御器ではない（`feedback_edge_node_scope` 方針）。遠隔オーバーライドは MQTT コマンド、現地はボタン。

---

## ハードウェア

| 項目 | 値 |
|---|---|
| MCU | M5Stack AtomS3 Lite（ESP32-S3） |
| ネット/給電 | Atomic PoE Base（K139, W5500, 802.3at PoE PD）。PoE 1本で給電＋通信 |
| リレー | M5Stack **Unit Relay（U002）**: 1ch **SPDT**（HK4100F）、**AC250V/3A または DC30V/3A**、5V コイル40mA |
| 接続 | Grove HY2.0-4P: **Yellow=DIN（制御, active HIGH）/ Red=5V / Black=GND / White=NC接点**。AtomS3 Grove に挿す |
| 制御ピン | **DIN = G2**（Grove 黄/SIG）既定。3.3V ロジック HIGH でユニット側ドライバをトリガ、コイルは Grove 5V 駆動。`/config` で変更可 |
| LED | 本体 WS2812C = **G35**（core の G27 ではない） |
| ボタン | 本体 = **G41**（AUTO / FORCE OFF / FORCE ON を巡回） |
| W5500 SPI | **SCK=5 / CS=6 / MISO=7 / MOSI=8**（AtomS3 + K139。classic ATOM の G22/23/33/19 ではない） |

⚠ DIN=G2 は Unit Relay の黄線定義からの既定。実機で動作未確認なら `/config` の「Relay DIN pin」で調整（agri-temp-poe は DS18B20 が白=G1 だった前例あり）。

## 制御モデル — 汎用1変数ヒステリシス・サーモスタット

入力値 `v`（`<src_prefix>/<src_category>/<input_type>` の agriha MQTT、任意で CCM）を監視し、**向き**付きヒステリシスでリレーを入り切り:

| 向き | 用途例 | ON | OFF | 設定 |
|---|---|---|---|---|
| **RAISE** | ヒーター / CO2 / 加湿 | `v ≤ on_point` | `v ≥ off_point` | on_point < off_point |
| **LOWER** | 換気 / 冷却 / 除湿 | `v ≥ on_point` | `v ≤ off_point` | on_point > off_point |

**判断の優先順位**（高い順、毎秒評価）:
1. **S1 ハード OFF**: FORCE OFF / 入力 stale（フェイルセーフ）/ hard-max・hard-min 限界 / max-on watchdog → 即 OFF
2. **S2 duty 枯渇** → OFF
3. **S3 min-off dwell** 未経過（ONしたい）→ OFF 維持
4. **S4 min-on dwell** 未経過（OFFしたい・現在ON）→ ON 保持
5. **N 通常**: FORCE ON、または ヒステリシス AND 時間窓 AND 外部ゲート

S1/S2 は安全・仕様上の上限で、S4 の min-on 保持も上書きする（迷ったら OFF）。boot 時は必ず de-energize。

**モード**: AUTO / FORCE ON / FORCE OFF。ボタン（G41）巡回・WebUI（Config の Mode）・MQTT コマンドで切替。

## MQTT

- **入力購読**: `<src_prefix>/<src_category>/<input_type>`（例 `agriha/2/sensor/InAirTemp`）、agriha `{value,...}` ペイロード。category は native=`sensor` / CCM ブリッジ経由=`sensor_ccm`。
- **CCM フォールバック**（`ccm_enabled` 時）: UDP 16520 の `<DATA type="<input_type>...">` を受信。
- **外部ゲート**（任意）: 別トピックの `{value}` を購読し、`value > gate_open_above` で ON を抑制（窓開ゲート等）。
- **状態 publish**（retain）: `<prefix>/relay/<relay_id>` に `{state:0/1, mode, reason, input, src, ts}`。リレー遷移時＋`mqtt_interval_s` ごと。
- **遠隔コマンド**: `<prefix>/relay/<relay_id>/cmd` に `{"mode":"auto|on|off"}` または `{"on":true/false}`。
- **LWT**: core 既定 `<prefix>/sys/<node_id>/online`。

## ビルド / 書き込み

```
pio run                       # ビルド
pio run -t upload             # 初回 USB-C（native USB CDC）
```
- platform = pioarduino フォーク `#55.03.38`（arduino-esp32 3.x）、board = `m5stack-atoms3`、core = `agri-node-poe-core@v0.7.0`。
- 以降は Ethernet 経由 ArduinoOTA（`agri-relay-poe-01.local`）/ WebUI `POST /api/ota` / GitHub Release セルフ更新（`yasunorioi/agri-relay-poe`、asset 名 `agri-relay-poe.bin`）。

## 設定（WebUI `http://agri-relay-poe-01.local/` または DHCP IP）

Config ページ: 共通（MQTT / CCM エンベロープ）＋ Relay（モード・DINピン・active HIGH・relay_id）＋ 入力（prefix/category/type・stale）＋ ヒステリシス（向き・ON/OFF点）＋ ハード限界（hi/lo）＋ 時間窓 ＋ 外部ゲート ＋ dwell/duty/watchdog。全て NVS 永続（`relayp-cfg`）。Dashboard に ON/OFF バナー・モード・理由・入力値・ルールを表示。

## 既定値（出荷時）

換気の例: 入力 `agriha/2/sensor/InAirTemp`、向き LOWER、ON=30℃/OFF=28℃、min on-off=30s、duty/watchdog なし、フェイルセーフ OFF、モード AUTO。用途に合わせ `/config` で自由に再ターゲット。

## 関連

- 共通ライブラリ: [agri-node-poe-core](https://github.com/yasunorioi/agri-node-poe-core)
- CO2 専用の自律リレー原型: [agri-co2-burner](https://github.com/yasunorioi/agri-co2-burner)
- 兄弟 PoE ノード: agri-env-poe / agri-temp-poe / agri-rain-poe / agri-flow-poe / agri-solar-poe / agri-uv-poe
- 別物の `agri-relay`（RP2350 の OGMS リレーノード）とは無関係。
