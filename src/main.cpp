#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <BLEDevice.h>  // core 付属の BLE（外付けの NimBLE-Arduino 1.4 は今の core と噛み合わず、起動時に落ちる＝tiepin で踏んだ）
#include <math.h>

// スマホ（Android の Chrome＝Web Bluetooth）から回し方・真ん中の文字・画像を変える。BLE の口は下の UUID・ページは docs/index.html。
// 変えた物は NVS に覚える＝電源を入れ直しても残る。画像はフラッシュ（LittleFS）に覚える。

#define TFT_RES  1
#define TFT_DC   2
#define TFT_SCLK 4
#define TFT_MOSI 6
#define TFT_CS   7

#define SVC_UUID   "5d650001-2f67-465b-9d3c-33c4607b0907"
#define SPIN_UUID  "5d650002-2f67-465b-9d3c-33c4607b0907"  // 3 バイト：速さ（1〜30・度/コマ）・向き（0=時計回り 1=反時計回り）・回す（0=止める 1=回す）
#define LABEL_UUID "5d650003-2f67-465b-9d3c-33c4607b0907"  // 真ん中の文字（ASCII・最大 24 バイト）
#define STYLE_UUID "5d650004-2f67-465b-9d3c-33c4607b0907"  // 書く 3 バイト：大きさ（1〜8）・色（RGB565 LE）
                                                            // 読む 4 バイト：上の 3 バイト＋実際の大きさ（輪の内に収まらなければ本体が縮める）
#define SHOW_UUID  "5d650005-2f67-465b-9d3c-33c4607b0907"  // 1 バイト：0=輪＋文字 1=輪＋真ん中に画像 2=画像だけ（全面）
#define IMAGE_UUID "5d650006-2f67-465b-9d3c-33c4607b0907"  // 書く：先頭の 1 バイトで何をするか
                                                            //   1, 種類（0=真ん中 1=全面）＝送り始め
                                                            //   2, 位置（uint32 LE）, 中身…＝続き（位置は前の続きでないと受けない）
                                                            //   3＝送り終わり（大きさが合えば差し替える）
                                                            // 読む 3 バイト：真ん中の画像がある・全面の画像がある・最後の送りの結果（0=よし 1=だめ 2=送っている途中）
// 画像は RGB565（LE）を左上から 1 行ずつ。真ん中は 164×164（丸の外は描かない）・全面は 240×240

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RES);

uint16_t sdgs_colors[17] = {
    0xE126, 0xD405, 0x2C4A, 0xC105, 0xEF44, 0x257D, 0xFCC0, 0xA106,
    0xF381, 0xE14A, 0xFCA2, 0xBF04, 0x3B67, 0x1B38, 0x5D28, 0x0413, 0x1209
};

const int LABEL_MAX = 24;
const int IMG_SIDE[2] = { 164, 240 };   // 真ん中・全面
const char *IMG_PATH[2] = { "/center.bin", "/full.bin" };
const char *IMG_TMP = "/upload.tmp";

enum Show : uint8_t { SHOW_LABEL = 0, SHOW_CENTER = 1, SHOW_FULL = 2, SHOW_COUNT };

float offset_angle = 0;

Preferences prefs;
volatile uint8_t spin_speed = 8;  // 度/コマ（元の固定値が 8）
volatile uint8_t spin_dir = 0;
volatile uint8_t spin_run = 1;
String label = "SDGs";
uint8_t label_size = 5;
uint16_t label_color = 0xFFFF;
uint8_t label_fit = 5;            // 実際に使う大きさ
volatile bool dirty = false;      // BLE から書かれた＝次の loop で NVS に保存する
// 文字は BLE の糸で受けて、描く糸（loop）で受け取る＝描いている最中に String を差し替えない
portMUX_TYPE label_mux = portMUX_INITIALIZER_UNLOCKED;
char pending_label[LABEL_MAX + 1];
volatile bool label_pending = false;
uint8_t pending_style[3];
volatile bool style_pending = false;
bool label_changed = true;        // 真ん中を一度黒で塗ってから描き直す（最初の 1 回も）

volatile uint8_t show = SHOW_LABEL;
volatile bool show_changed = true;  // 画面をまっさらにしてから描き直す（最初の 1 回も）
bool image_changed = true;
// 画像の受け取り：中身は BLE の糸で一時ファイルへ書き、差し替えは描く糸（loop）でやる＝描いている最中に消さない
File upload;
int upload_kind = -1;
uint32_t upload_pos = 0;
volatile int commit_kind = -1;    // 送り終わった画像の種類＝loop で差し替える
uint8_t upload_status = 0;

BLECharacteristic *ch_spin, *ch_label, *ch_style, *ch_show, *ch_image;

void setupImageValue() {
  uint8_t v[3] = { LittleFS.exists(IMG_PATH[0]), LittleFS.exists(IMG_PATH[1]), upload_status };
  ch_image->setValue(v, 3);
}

void onImageWrite(const uint8_t *v, size_t len) {
  if (len >= 2 && v[0] == 1) {
    if (upload) upload.close();
    upload_kind = v[1] ? 1 : 0;
    upload_pos = 0;
    upload = LittleFS.open(IMG_TMP, "w");
    upload_status = upload ? 2 : 1;
  } else if (len >= 5 && v[0] == 2) {
    uint32_t pos;
    memcpy(&pos, v + 1, 4);
    if (!upload || pos != upload_pos || upload.write(v + 5, len - 5) != len - 5) {
      if (upload) upload.close();
      upload_status = 1;  // 抜けた・ずれた＝はじめから送り直してもらう
      return setupImageValue();
    }
    upload_pos += len - 5;
    return;  // 続きの度に値を書き換えない（遅くなる）
  } else if (len >= 1 && v[0] == 3) {
    size_t want = IMG_SIDE[upload_kind < 0 ? 0 : upload_kind] * IMG_SIDE[upload_kind < 0 ? 0 : upload_kind] * 2;
    bool ok = upload && upload_kind >= 0 && upload_pos == want;
    if (upload) upload.close();
    upload_status = ok ? 0 : 1;
    if (ok) commit_kind = upload_kind;
    upload_kind = -1;
  }
  setupImageValue();
}

void setupSpinValue() {
  uint8_t v[3] = { spin_speed, spin_dir, spin_run };
  ch_spin->setValue(v, 3);
}

// 文字が輪の内側（半径 80 くらい）に収まる一番大きい大きさを選ぶ（loop の中で呼ぶ＝tft を触るので）
void applyLabel() {
  label_fit = label_size;
  for (; label_fit > 1; label_fit--) {
    int16_t x1, y1;
    uint16_t w, h;
    tft.setTextSize(label_fit);
    tft.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
    if ((w / 2.0f) * (w / 2.0f) + (h / 2.0f) * (h / 2.0f) <= 80 * 80) break;
  }
  uint8_t s[4] = { label_size, (uint8_t)(label_color & 0xFF), (uint8_t)(label_color >> 8), label_fit };
  if (ch_style) ch_style->setValue(s, 4);
  if (ch_label) ch_label->setValue(label);
  label_changed = true;
}

class Writes : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    const uint8_t *v = c->getData();
    size_t len = c->getLength();
    if (c == ch_spin && len >= 3) {
      spin_speed = constrain(v[0], 1, 30);
      spin_dir = v[1] ? 1 : 0;
      spin_run = v[2] ? 1 : 0;
      setupSpinValue();
    } else if (c == ch_label) {
      portENTER_CRITICAL(&label_mux);
      size_t n = min((size_t)LABEL_MAX, len);
      for (size_t i = 0; i < n; i++) pending_label[i] = v[i] < 0x80 ? v[i] : '?';  // 本体のフォントは ASCII だけ
      pending_label[n] = 0;
      label_pending = true;
      portEXIT_CRITICAL(&label_mux);
    } else if (c == ch_show && len >= 1 && v[0] < SHOW_COUNT) {
      show = v[0];
      show_changed = true;
    } else if (c == ch_image) {
      onImageWrite(v, len);
      return;  // 画像は NVS に覚えない
    } else if (c == ch_style && len >= 3) {
      portENTER_CRITICAL(&label_mux);
      memcpy(pending_style, v, 3);
      style_pending = true;
      portEXIT_CRITICAL(&label_mux);
    } else {
      return;
    }
    dirty = true;
  }
};

// つながっている間も名乗り続ける＝前のつながりが裏で残っていても、つなぎ直せる
class Conn : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { BLEDevice::startAdvertising(); }
  void onDisconnect(BLEServer *s) override { BLEDevice::startAdvertising(); }
};

void setupBle() {
  BLEDevice::init("sdgs");
  BLEDevice::setMTU(517);  // 画像を大きめの切れで受ける（スマホが 517 まで上げてくる）
  BLEServer *srv = BLEDevice::createServer();
  srv->setCallbacks(new Conn());
  BLEService *svc = srv->createService(SVC_UUID);
  static Writes cb;
  const uint32_t RW = BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE;
  ch_spin  = svc->createCharacteristic(SPIN_UUID, RW);
  ch_label = svc->createCharacteristic(LABEL_UUID, RW);
  ch_style = svc->createCharacteristic(STYLE_UUID, RW);
  ch_show  = svc->createCharacteristic(SHOW_UUID, RW);
  ch_image = svc->createCharacteristic(IMAGE_UUID, RW);
  for (BLECharacteristic *c : { ch_spin, ch_label, ch_style, ch_show, ch_image }) c->setCallbacks(&cb);
  // 今の値を読めるようにしておく（ページが開いた時に今の設定を表示する）
  setupSpinValue();
  applyLabel();
  uint8_t sh = show;
  ch_show->setValue(&sh, 1);
  setupImageValue();
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void setup() {
    Serial.begin(115200);
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(0x0000);

    prefs.begin("sdgs", false);
    spin_speed = constrain(prefs.getUChar("speed", 8), 1, 30);
    spin_dir = prefs.getUChar("dir", 0) ? 1 : 0;
    spin_run = prefs.getUChar("run", 1) ? 1 : 0;
    label = prefs.getString("label", "SDGs");
    label_size = constrain(prefs.getUChar("size", 5), 1, 8);
    label_color = prefs.getUShort("color", 0xFFFF);
    show = prefs.getUChar("show", SHOW_LABEL);
    if (show >= SHOW_COUNT) show = SHOW_LABEL;

    if (!LittleFS.begin(true)) Serial.println("LittleFS が使えない（画像は覚えられない）");

    setupBle();
}

void drawSDGsArc(int cx, int cy, int r_in, int r_out, float start_deg, float end_deg, uint16_t color) {
    float s_rad = start_deg * M_PI / 180.0;
    float e_rad = (end_deg + 1.1) * M_PI / 180.0; // 隙間埋め

    int x1 = cx + cos(s_rad) * r_in;
    int y1 = cy + sin(s_rad) * r_in;
    int x2 = cx + cos(s_rad) * r_out;
    int y2 = cy + sin(s_rad) * r_out;
    int x3 = cx + cos(e_rad) * r_in;
    int y3 = cy + sin(e_rad) * r_in;
    int x4 = cx + cos(e_rad) * r_out;
    int y4 = cy + sin(e_rad) * r_out;

    tft.fillTriangle(x1, y1, x2, y2, x3, y3, color);
    tft.fillTriangle(x2, y2, x3, y3, x4, y4, color);
}

void takePending() {
    if (label_pending || style_pending) {
        char buf[LABEL_MAX + 1];
        uint8_t st[3];
        bool got_label, got_style;
        portENTER_CRITICAL(&label_mux);
        memcpy(buf, pending_label, sizeof(buf));
        memcpy(st, pending_style, 3);
        got_label = label_pending;
        got_style = style_pending;
        label_pending = style_pending = false;
        portEXIT_CRITICAL(&label_mux);
        if (got_label) label = String(buf);
        if (got_style) {
            label_size = constrain(st[0], 1, 8);
            label_color = st[1] | (st[2] << 8);
        }
        applyLabel();
    }
    if (dirty) {  // NVS への書き込みは BLE の割り込みの外で
        dirty = false;
        prefs.putUChar("speed", spin_speed);
        prefs.putUChar("dir", spin_dir);
        prefs.putUChar("run", spin_run);
        prefs.putString("label", label);
        prefs.putUChar("size", label_size);
        prefs.putUShort("color", label_color);
        prefs.putUChar("show", show);
    }
    if (commit_kind >= 0) {  // 送り終わった画像を差し替える
        int k = commit_kind;
        commit_kind = -1;
        LittleFS.remove(IMG_PATH[k]);
        LittleFS.rename(IMG_TMP, IMG_PATH[k]);
        image_changed = true;
        setupImageValue();
    }
    if (show_changed) {
        show_changed = false;
        uint8_t sh = show;
        ch_show->setValue(&sh, 1);
        tft.fillScreen(0x0000);
        label_changed = image_changed = true;
    }
}

// 画像をファイルから 1 行ずつ描く。丸の外は描かない（真ん中の画像の角が輪に掛からないように）
void drawImage(int kind) {
    int side = IMG_SIDE[kind], x0 = 120 - side / 2, r = side / 2;
    File f = LittleFS.open(IMG_PATH[kind], "r");
    if (!f) return;  // まだ送られていない＝黒のまま
    static uint16_t row[240];
    for (int y = 0; y < side && f.read((uint8_t *)row, side * 2) == side * 2; y++) {
        float dy = y + 0.5f - r;
        int half = (int)sqrtf(max(0.0f, r * r - dy * dy));
        tft.drawRGBBitmap(x0 + r - half, x0 + y, row + r - half, half * 2, 1);
    }
    f.close();
}

void loop() {
    takePending();

    if (show == SHOW_FULL) {  // 画像だけ＝変わった時に一度描けば済む
        if (image_changed) { image_changed = false; drawImage(1); }
        delay(20);
        return;
    }

    int cx = 120, cy = 120;
    int r_out_target = 118; // 本当に見せたい外径
    int r_in_target = 85;   // 本当に見せたい内径

    float step = 360.0 / 17.0;

    // 文字が変わった時だけ、真ん中を黒で塗り直す（毎コマ塗るとチラつく）
    if (show == SHOW_LABEL && label_changed) {
        label_changed = false;
        tft.fillCircle(cx, cy, r_in_target - 2, 0x0000);
    }

    // 1. カラー部分の描画（ハミ出しを極小にする）
    for (int i = 0; i < 17; i++) {
        drawSDGsArc(cx, cy, r_in_target, r_out_target + 2,
                    i * step + offset_angle,
                    (i + 1) * step + offset_angle,
                    sdgs_colors[i]);
    }

    // 2. 外周の「震え」防止（カラーを塗った直後に、その外側を黒で即座に殺す）
    // 描画範囲の外側（120〜122）を常に黒で固定
    tft.drawCircle(cx, cy, 121, 0x0000);
    tft.drawCircle(cx, cy, 122, 0x0000);
    tft.drawCircle(cx, cy, r_out_target + 1, 0xFFFF); // 外枠白線

    // 3. 内周の安定化（中央の文字部分を保護）
    // fillCircleを毎フレーム呼ぶと文字が消えるため、
    // 「色を塗った直後の内側エッジ」だけを白線で叩く
    tft.drawCircle(cx, cy, r_in_target - 1, 0xFFFF);
    tft.drawCircle(cx, cy, r_in_target - 2, 0x0000); // 色が内側へ滲むのを防ぐ黒線

    // 4. 文字を最後に描画して、上書きされないようにする（輪の真ん中に揃える）
    if (show == SHOW_LABEL) {
        int16_t x1, y1;
        uint16_t w, h;
        tft.setTextSize(label_fit);
        tft.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
        tft.setCursor(cx - (int)w / 2 - x1, cy - (int)h / 2 - y1);
        tft.setTextColor(label_color);
        tft.print(label);
    } else if (image_changed) {  // 真ん中の画像も変わった時に一度だけ（輪は内側の黒線より外しか塗らない）
        image_changed = false;
        tft.fillCircle(cx, cy, r_in_target - 2, 0x0000);
        drawImage(0);
    }

    if (spin_run) {
        offset_angle += spin_dir ? -spin_speed : spin_speed;
        if (offset_angle >= 360) offset_angle -= 360;
        if (offset_angle < 0) offset_angle += 360;
    }

    delay(2);
}
