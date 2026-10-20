#include "board.h"
#ifdef BOARD_FREENOVE_S3
// Freenove FNK0104S: ST7796 480x320, FT6336U capacitive touch, ES8311 codec + speaker.
#include <Wire.h>
#include "driver/i2s.h"
#include "FT6336U.h"
#include "es8311.h"
#include "hal.h"
#include "audio.h"

extern es8311_handle_t g_es_handle;

#define I2C_SDA 16
#define I2C_SCL 15
#define RST_N_PIN 18
#define INT_N_PIN 17
#define AP_ENABLE 1
#define I2S_MCK 4
#define I2S_BCK 5
#define I2S_DINT 6
#define I2S_DOUT 8
#define I2S_WS 7

static FT6336U ft6336u(I2C_SDA, I2C_SCL, RST_N_PIN, INT_N_PIN);

void halBacklight(bool on) { digitalWrite(TFT_BL, on ? HIGH : LOW); }

// The rotation's MADCTL as TFT_eSPI sets it for the ST7796, with red/blue optionally flipped.
void halApplyColors() {
  static const uint8_t baseFor[4] = {TFT_MAD_MX, TFT_MAD_MV, TFT_MAD_MY, TFT_MAD_MX | TFT_MAD_MY | TFT_MAD_MV};
  const uint8_t base = baseFor[tft.getRotation() & 3];
  const uint8_t order = g_swaprb ? (TFT_MAD_COLOR_ORDER ^ TFT_MAD_BGR) : TFT_MAD_COLOR_ORDER;
  tft.writecommand(TFT_MADCTL);
  tft.writedata(base | order);
#ifdef TFT_INVERSION_ON
  tft.invertDisplay(!g_invert);
#else
  tft.invertDisplay(g_invert);
#endif
}

void halApplyRotation() {
  tft.setRotation(g_rot);
  halApplyColors();
}

void halDisplayBegin() {
  tft.init();
  halApplyRotation();
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);
}

void halTouchBegin() { ft6336u.begin(); }

bool halTouch(int &x, int &y) {
  FT6336U_TouchPointType tp = ft6336u.scan();
  if (tp.touch_count == 0) return false;
  x = g_rot == 3 ? PANEL_W - tp.tp[0].y : tp.tp[0].y;
  y = g_rot == 3 ? tp.tp[0].x : PANEL_H - tp.tp[0].x;
  return true;
}

void halRecalibrateTouch() {}

void halApplySound() {
  digitalWrite(AP_ENABLE, g_snd ? LOW : HIGH);          // LOW = amp on (as in the Freenove demos)
  if (g_es_handle) es8311_voice_mute(g_es_handle, !g_snd);
}

static void audioTask(void *) {
  static int16_t mono[256], out[512];
  for (;;) {
    audioMix(mono, 256);
    for (int i = 0; i < 256; i++) { out[2 * i] = mono[i]; out[2 * i + 1] = mono[i]; }
    size_t written;
    i2s_write(I2S_NUM_0, out, sizeof(out), &written, portMAX_DELAY);
  }
}

void halAudioStart() {
  pinMode(AP_ENABLE, OUTPUT);
  digitalWrite(AP_ENABLE, LOW);
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = EXAMPLE_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = EXAMPLE_MCLK_FREQ_HZ
  };
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) { Serial.println("I2S install failed"); return; }
  i2s_pin_config_t pins = {.mck_io_num = I2S_MCK, .bck_io_num = I2S_BCK, .ws_io_num = I2S_WS,
                           .data_out_num = I2S_DOUT, .data_in_num = I2S_DINT};
  i2s_set_pin(I2S_NUM_0, &pins);
  if (es8311_codec_init() != ESP_OK) { Serial.println("ES8311 init failed"); return; }
  halApplySound();
  xTaskCreatePinnedToCore(audioTask, "bubbles", 4096, NULL, 2, NULL, 0);
}
#endif
