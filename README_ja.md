# agri-relay-poe

**🇯🇵 日本語** ・ 🇬🇧 [English](README_en.md)

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

リンク層は2つの env で切替（1ファームを共有）:
```
# PoE / Ethernet 版（既定・ハウス内推奨）:
pio run -e m5atoms3-poe -t upload

# WiFi 版（AtomS3 内蔵 WiFi、W5500 不使用、USB-C/5V 給電、LAN ドロップが無い場所用）:
pio run -e m5atoms3-wifi -t upload

# house2 機（CO2 が CCM ブリッジ経由 = agriha/2 / sensor_ccm、node=relay_poe_02）は
# どちらの env にも -DHOUSE2 を足す:
PLATFORMIO_BUILD_FLAGS="-DHOUSE2" pio run -e m5atoms3-poe -t upload
```
初回は USB-C（native USB CDC）。以降は Ethernet/WiFi 経由 OTA。
> ⚠ 既定(setDefaults)を変えて焼くときは **NVS に残る旧設定が上書きする**ので、先に
> `python -m esptool --port <port> erase_flash` してから upload。最終的な各ノード設定は
> 現地 LAN 上の WebUI `/config`（NVS 永続）で詰める。

### WiFi 版のプロビジョニング（m5atoms3-wifi）
- WiFiManager が WiFi 資格情報と設定ポータルを管理。creds 未設定なら SoftAP **`agri-relay-setup`** が立つ → 接続して WiFi＋**MQTT host/port/prefix** を入力。以降は自動接続（ポータルはタイムアウトするのでオフラインでも制御は動く＝フェイルセーフ OFF）。
- **ボタン(G41)を押しながら起動**で再プロビジョニング。詳細 src/しきい値は接続後に WebUI `/config` で。
- ハウス内は EMI/長距離で無線が不安定寄り → **基本 PoE 優先、WiFi は代替**。
- platform = pioarduino フォーク `#55.03.38`（arduino-esp32 3.x）、board = `m5stack-atoms3`、core = `agri-node-poe-core@v0.7.0`。
- 以降は Ethernet 経由 ArduinoOTA（`agri-relay-poe-01.local`）/ WebUI `POST /api/ota` / GitHub Release セルフ更新（`yasunorioi/agri-relay-poe`、asset 名は PoE 版 `agri-relay-poeco2.bin` / WiFi 版 `agri-relay-wifico2.bin`）。

### リリース / セルフ更新(OTA)手順
AgriOTA は起動時＋日次で `releases/latest` を見て、ビルドごとの asset（PoE=`agri-relay-poeco2.bin` / WiFi=`agri-relay-wifico2.bin`）を**ユーザのクリック時のみ**フラッシュ(WebUI の更新ボタン / `POST /api/update`)。新版の出し方:
```
# src/main.cpp の FW_VERSION を上げる（例 0.1.2）→ commit
pio run -e m5atoms3-poe -e m5atoms3-wifi
cp .pio/build/m5atoms3-poe/firmware.bin  agri-relay-poeco2.bin
cp .pio/build/m5atoms3-wifi/firmware.bin agri-relay-wifico2.bin
gh release create v0.1.2 --target main --title v0.1.2 \
  --notes "…" agri-relay-poeco2.bin agri-relay-wifico2.bin
```
- タグは必ず `vX.Y.Z`、asset 名は厳密に上の2つ(違うと tag は見えても DL が 404)。**両方必ず添付** — 片方欠けるとそのビルドの機体は更新できない。
- v0.1.1 以前は両ビルドとも `agri-relay-poe.bin`(=PoE 版)を取りに行く。v0.1.1 以前の **WiFi 機は更新ボタンを押さず**、`agri-relay-wifico2.bin` を `POST /api/ota` で入れること(PoE 版が入るとネットワークを失う)。
- OTA イメージは1個の汎用ビルド(既定=house3)。**各ノードの house2/3 の別は NVS で保つ** — 現地で一度 WebUI `/config` を保存しておけば、OTA 更新しても設定は維持される(保存前に OTA すると既定の house3 に戻るので注意)。
- OTA チェックは api.github.com への到達が必要(現地のインターネット回線)。

## 設定（WebUI `http://agri-relay-poe-01.local/` または DHCP IP）

Config ページ: 共通（MQTT / CCM エンベロープ）＋ Relay（モード・DINピン・active HIGH・relay_id）＋ 入力（prefix/category/type・stale）＋ ヒステリシス（向き・ON/OFF点）＋ ハード限界（hi/lo）＋ 時間窓 ＋ 外部ゲート ＋ dwell/duty/watchdog。全て NVS 永続（`relayp-cfg`）。Dashboard に ON/OFF バナー・モード・理由・入力値・ルールを表示。

## 既定値（出荷時 = CO2バーナー投入プロファイル）

初回デプロイ向けに、**agri-co2-burner の実績値を写した CO2 投入プロファイル**を既定とする（CG-1000 を Unit Relay で駆動する想定）。汎用ノードなので `/config` で自由に再ターゲット可。

| 項目 | 既定 | 意味 |
|---|---|---|
| 入力 | `agriha/3/sensor/InAirCO2` | house3 の CO2（native=sensor）。house2 は CCM ブリッジ経由なので `sensor_ccm` に |
| 向き | **RAISE** | CO2 を上げる＝ON when CO2 ≤ on_point |
| ON / OFF | 450 / 520 ppm | CO2≤450 で投入、≥520 で停止 |
| ハード限界 | **≥1000 ppm → OFF**（常時適用） | CG-1000 仕様。FORCE ON でも値が新鮮なら上書き |
| 時間帯 | 08:00–15:00 | 日中のみ（光合成）。要調整 |
| dwell | min on/off = 300s | 短サイクル防止 |
| duty | 50% / 3600s 窓 | CG-1000 デューティ ≤50%/時 |
| stale | 180s | CO2 が古い → フェイルセーフ OFF |
| 状態 publish | `agriha/3/relay/co2` | retain |
| モード | AUTO | **再起動時 FORCE_ON は AUTO に降格**（無人再起動で燃焼器を点けない安全） |
| 窓ゲート | OFF | co2-burner は ArSprout HTTP で窓開を見て抑制。本機は MQTT `{value}/{pct}` トピックで代替可（既定 OFF、`gate_topic` に窓トピックを指定して有効化） |

⚠ **既存 agri-co2-burner（WiFi）も house3 の CG-1000 を制御している**。CG-1000 を駆動するコントローラは**この PoE ノードか WiFi 版のどちらか一方**にすること（二重駆動しない）。

別用途（ヒーター/換気/灌水 等）に使うときは `/config` で 入力・向き・ON/OFF点・限界・duty を変えるだけ。

## 関連

- 共通ライブラリ: [agri-node-poe-core](https://github.com/yasunorioi/agri-node-poe-core)
- CO2 専用の自律リレー原型: [agri-co2-burner](https://github.com/yasunorioi/agri-co2-burner)
- 兄弟 PoE ノード: agri-env-poe / agri-temp-poe / agri-rain-poe / agri-flow-poe / agri-solar-poe / agri-uv-poe
- 別物の `agri-relay`（RP2350 の OGMS リレーノード）とは無関係。
