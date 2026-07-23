#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <math.h>

#define TFT_RES  1
#define TFT_DC   2
#define TFT_SCLK 4
#define TFT_MOSI 6
#define TFT_CS   7

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RES);

uint16_t sdgs_colors[17] = {
    0xE126, 0xD405, 0x2C4A, 0xC105, 0xEF44, 0x257D, 0xFCC0, 0xA106, 
    0xF381, 0xE14A, 0xFCA2, 0xBF04, 0x3B67, 0x1B38, 0x5D28, 0x0413, 0x1209
};

float offset_angle = 0;

void setup() {
    Serial.begin(115200);
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(0x0000);
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

void loop() {
    int cx = 120, cy = 120;
    int r_out_target = 118; // 本当に見せたい外径
    int r_in_target = 85;   // 本当に見せたい内径
    
    float step = 360.0 / 17.0;

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

    // 4. 文字（SDGs）を最後に描画して、上書きされないようにする
    tft.setCursor(65, 100);
    tft.setTextColor(0xFFFF);
    tft.setTextSize(5);
    tft.print("SDGs");

    offset_angle += 8.0; 
    if (offset_angle >= 360) offset_angle -= 360;

    delay(2); 
}
