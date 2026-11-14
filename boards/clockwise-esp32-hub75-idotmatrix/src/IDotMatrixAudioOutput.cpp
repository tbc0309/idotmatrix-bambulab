#include "IDotMatrixAudioOutput.h"

#if IDOTMATRIX_AUDIO_CODEC_AVAILABLE

#include <Wire.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
i2s_chan_handle_t gTxChannel = nullptr;
TwoWire gAudioWire(1);
constexpr uint32_t kSampleRate = 48000;
constexpr size_t kFramesPerBlock = 192;
constexpr int16_t kAmplitude = 9000;  // Qualified digital headroom; codec gain is configured separately.

volatile bool gToneOn = false;
volatile bool gReady = false;
TaskHandle_t gAudioTask = nullptr;
uint32_t gPhase = 0;
bool gFirstWriteReported = false;

struct CodecCoeff {
  uint32_t mclk;
  uint32_t rate;
  uint8_t preDiv;
  uint8_t preMulti;
  uint8_t adcDiv;
  uint8_t dacDiv;
  uint8_t fsMode;
  uint8_t lrckH;
  uint8_t lrckL;
  uint8_t bclkDiv;
  uint8_t adcOsr;
  uint8_t dacOsr;
};

// Official Waveshare ESP32-S3-RGB-Matrix ES8311 48 kHz coefficient.
constexpr CodecCoeff kCodec48k = {
  12288000U, 48000U,
  0x01, 0x00, 0x01, 0x01,
  0x00, 0x00, 0xFF, 0x04,
  0x10, 0x10
};

bool writeReg(uint8_t reg, uint8_t value) {
  gAudioWire.beginTransmission(IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS);
  gAudioWire.write(reg);
  gAudioWire.write(value);
  const uint8_t rc = gAudioWire.endTransmission();
  if (rc != 0) {
    Serial.print("AUDIO: ES8311 write failed reg=0x"); Serial.print(reg, HEX);
    Serial.print(" rc="); Serial.println(rc);
  }
  return rc == 0;
}

bool readReg(uint8_t reg, uint8_t &value) {
  gAudioWire.beginTransmission(IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS);
  gAudioWire.write(reg);
  const uint8_t txRc = gAudioWire.endTransmission(false);
  if (txRc != 0) {
    Serial.print("AUDIO: ES8311 address/read-select failed reg=0x"); Serial.print(reg, HEX);
    Serial.print(" rc="); Serial.println(txRc);
    return false;
  }
  const uint8_t got = gAudioWire.requestFrom((uint16_t)IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS, (uint8_t)1, true);
  if (got != 1) {
    Serial.print("AUDIO: ES8311 read failed reg=0x"); Serial.print(reg, HEX);
    Serial.print(" bytes="); Serial.println(got);
    return false;
  }
  value = gAudioWire.read();
  return true;
}


bool codecSetSampleRate48k() {
  const CodecCoeff &selected = kCodec48k;
  uint8_t reg = 0;
  if (!readReg(0x02, reg)) return false;
  reg |= (uint8_t)((selected.preDiv - 1U) << 5);
  reg |= (uint8_t)(selected.preMulti << 3);
  if (!writeReg(0x02, reg)) return false;

  const uint8_t reg03 = (uint8_t)((selected.fsMode << 6) | selected.adcOsr);
  if (!writeReg(0x03, reg03)) return false;
  if (!writeReg(0x04, selected.dacOsr)) return false;

  const uint8_t reg05 = (uint8_t)(((selected.adcDiv - 1U) << 4) | (selected.dacDiv - 1U));
  if (!writeReg(0x05, reg05)) return false;

  if (!readReg(0x06, reg)) return false;
  reg &= 0xE0U;
  reg |= (selected.bclkDiv < 19U) ? (uint8_t)(selected.bclkDiv - 1U) : selected.bclkDiv;
  if (!writeReg(0x06, reg)) return false;

  if (!readReg(0x07, reg)) return false;
  reg &= 0xC0U;
  reg |= selected.lrckH;
  if (!writeReg(0x07, reg)) return false;
  return writeReg(0x08, selected.lrckL);
}

bool codecSetBits16() {
  uint8_t reg09 = 0;
  uint8_t reg0A = 0;
  if (!readReg(0x09, reg09) || !readReg(0x0A, reg0A)) return false;
  // Preserve the official Waveshare driver behavior for 16-bit words.
  reg09 |= (3U << 2);
  reg0A |= (3U << 2);
  return writeReg(0x09, reg09) && writeReg(0x0A, reg0A);
}

bool codecSetVolume(uint8_t volume) {
  if (volume > 100U) volume = 100U;
  int reg32 = 0;
  if (volume != 0U) reg32 = ((int)volume * 256 / 100) - 1;
  const bool ok = writeReg(0x32, (uint8_t)reg32);
  if (ok) {
    Serial.print("AUDIO: ES8311 codec volume="); Serial.print(volume);
    Serial.print("% reg32=0x"); if (reg32 < 0x10) Serial.print('0'); Serial.println(reg32, HEX);
  }
  return ok;
}

bool codecBegin() {
  gAudioWire.beginTransmission(IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS);
  const uint8_t probeRc = gAudioWire.endTransmission();
  if (probeRc != 0) {
    Serial.print("AUDIO: ES8311 probe failed addr=0x"); Serial.print(IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS, HEX);
    Serial.print(" rc="); Serial.println(probeRc);
    return false;
  }
  Serial.print("AUDIO: ES8311 probe OK addr=0x"); Serial.println(IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS, HEX);

  // Keep this sequence aligned with the official Waveshare
  // ESP32-S3-RGB-Matrix Arduino Music Player ES8311 driver.
  bool ok = true;
  uint8_t reg = 0;
  ok &= writeReg(0x00, 0x1F); // reset
  delay(20);
  ok &= writeReg(0x00, 0x00); // release reset
  ok &= writeReg(0x00, 0x80); // power on
  ok &= writeReg(0x01, 0x3F); // enable all clocks

  if (!readReg(0x06, reg)) return false;
  reg &= ~(1U << 5);          // BCLK not inverted
  ok &= writeReg(0x06, reg);
  ok &= codecSetSampleRate48k();
  ok &= codecSetBits16();
  ok &= writeReg(0x0D, 0x01); // power up analog circuitry
  ok &= writeReg(0x0E, 0x02); // analog PGA + ADC modulator
  ok &= writeReg(0x12, 0x00); // power up DAC
  ok &= writeReg(0x13, 0x10); // output to HP driver
  ok &= writeReg(0x1C, 0x6A); // ADC EQ bypass / DC cancellation
  ok &= writeReg(0x37, 0x08); // DAC EQ bypass
  ok &= codecSetVolume(IDOTMATRIX_AUDIO_CODEC_VOLUME);
  if (!ok) return false;

  Serial.print("AUDIO: ES8311 configured, volume=");
  Serial.print(IDOTMATRIX_AUDIO_CODEC_VOLUME);
  Serial.println("%");
  return true;
}

bool i2sBegin() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  esp_err_t err = i2s_new_channel(&chanCfg, &gTxChannel, nullptr);
  if (err != ESP_OK || gTxChannel == nullptr) {
    Serial.print("AUDIO: i2s_new_channel failed err="); Serial.print((int)err);
    Serial.print(" ("); Serial.print(esp_err_to_name(err)); Serial.println(")");
    gTxChannel = nullptr;
    return false;
  }

  i2s_std_config_t stdCfg = {};
  const i2s_std_clk_config_t clkCfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  const i2s_std_slot_config_t slotCfg =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  stdCfg.clk_cfg = clkCfg;
  stdCfg.slot_cfg = slotCfg;
  stdCfg.gpio_cfg.mclk = (gpio_num_t)IDOTMATRIX_AUDIO_I2S_MCLK_PIN;
  stdCfg.gpio_cfg.bclk = (gpio_num_t)IDOTMATRIX_AUDIO_I2S_BCLK_PIN;
  stdCfg.gpio_cfg.ws = (gpio_num_t)IDOTMATRIX_AUDIO_I2S_WS_PIN;
  stdCfg.gpio_cfg.dout = (gpio_num_t)IDOTMATRIX_AUDIO_I2S_DOUT_PIN;
  stdCfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.ws_inv = false;

  err = i2s_channel_init_std_mode(gTxChannel, &stdCfg);
  if (err != ESP_OK) {
    Serial.print("AUDIO: i2s_channel_init_std_mode failed err="); Serial.print((int)err);
    Serial.print(" ("); Serial.print(esp_err_to_name(err)); Serial.println(")");
    i2s_del_channel(gTxChannel);
    gTxChannel = nullptr;
    return false;
  }

  err = i2s_channel_enable(gTxChannel);
  if (err != ESP_OK) {
    Serial.print("AUDIO: i2s_channel_enable failed err="); Serial.print((int)err);
    Serial.print(" ("); Serial.print(esp_err_to_name(err)); Serial.println(")");
    i2s_del_channel(gTxChannel);
    gTxChannel = nullptr;
    return false;
  }
  Serial.println("AUDIO: I2S1 TX channel enabled");
  return true;
}

void i2sEnd() {
  if (gTxChannel == nullptr) return;
  i2s_channel_disable(gTxChannel);
  i2s_del_channel(gTxChannel);
  gTxChannel = nullptr;
}

void audioTask(void *) {
  int16_t stereo[kFramesPerBlock * 2];
  const uint32_t phaseStep = (uint32_t)(((uint64_t)IDOTMATRIX_BUZZER_FREQUENCY_HZ << 32) / kSampleRate);
  for (;;) {
    const bool tone = gToneOn;
    for (size_t i = 0; i < kFramesPerBlock; ++i) {
      int16_t sample = 0;
      if (tone) {
        sample = (gPhase & 0x80000000UL) ? kAmplitude : -kAmplitude;
        gPhase += phaseStep;
      }
      stereo[i * 2] = sample;
      stereo[i * 2 + 1] = sample;
    }
    size_t written = 0;
    const esp_err_t err = i2s_channel_write(gTxChannel, stereo, sizeof(stereo), &written, 1000);
    if (err != ESP_OK || written != sizeof(stereo)) {
      Serial.print("AUDIO: I2S write issue err="); Serial.print((int)err);
      Serial.print(" written="); Serial.print(written);
      Serial.print(" expected="); Serial.println(sizeof(stereo));
      taskYIELD();
    } else if (!gFirstWriteReported) {
      gFirstWriteReported = true;
      Serial.print("AUDIO: I2S TX first block OK bytes="); Serial.println(written);
    }
  }
}
} // namespace

bool idotAudioOutputBegin() {
  if (gReady) return true;

  Serial.print("AUDIO: begin ES8311=0x"); Serial.print(IDOTMATRIX_AUDIO_CODEC_I2C_ADDRESS, HEX);
  Serial.print(" I2C1 SDA="); Serial.print(IDOTMATRIX_AUDIO_I2C_SDA_PIN);
  Serial.print(" SCL="); Serial.print(IDOTMATRIX_AUDIO_I2C_SCL_PIN);
  Serial.print(" I2S1 MCLK="); Serial.print(IDOTMATRIX_AUDIO_I2S_MCLK_PIN);
  Serial.print(" BCLK="); Serial.print(IDOTMATRIX_AUDIO_I2S_BCLK_PIN);
  Serial.print(" WS="); Serial.print(IDOTMATRIX_AUDIO_I2S_WS_PIN);
  Serial.print(" DOUT="); Serial.print(IDOTMATRIX_AUDIO_I2S_DOUT_PIN);
  Serial.print(" PA="); Serial.println(IDOTMATRIX_AUDIO_PA_ENABLE_PIN);

  if (!gAudioWire.begin(IDOTMATRIX_AUDIO_I2C_SDA_PIN, IDOTMATRIX_AUDIO_I2C_SCL_PIN, 400000U)) {
    Serial.println("AUDIO: I2C1 begin failed");
    return false;
  }
  Serial.println("AUDIO: I2C1 ready");

  // The official Waveshare Music Player enables the external amplifier before
  // codec initialization; keep the same ordering on the qualified board.
  pinMode(IDOTMATRIX_AUDIO_PA_ENABLE_PIN, OUTPUT);
  digitalWrite(IDOTMATRIX_AUDIO_PA_ENABLE_PIN, HIGH);
  Serial.print("AUDIO: PA GPIO"); Serial.print(IDOTMATRIX_AUDIO_PA_ENABLE_PIN); Serial.println(" -> HIGH");

  if (!codecBegin()) {
    digitalWrite(IDOTMATRIX_AUDIO_PA_ENABLE_PIN, LOW);
    Serial.println("AUDIO: ES8311 init failed; PA -> LOW");
    return false;
  }
  if (!i2sBegin()) {
    digitalWrite(IDOTMATRIX_AUDIO_PA_ENABLE_PIN, LOW);
    Serial.println("AUDIO: I2S1 TX init failed; PA -> LOW");
    return false;
  }

  if (xTaskCreatePinnedToCore(audioTask, "idot_audio", 3072, nullptr, 1, &gAudioTask, 1) != pdPASS) {
    i2sEnd();
    digitalWrite(IDOTMATRIX_AUDIO_PA_ENABLE_PIN, LOW);
    Serial.println("AUDIO: producer task creation failed; PA -> LOW");
    return false;
  }

  gReady = true;
  Serial.print("AUDIO: ES8311/I2S1 ready, synthesized square ");
  Serial.print(IDOTMATRIX_BUZZER_FREQUENCY_HZ);
  Serial.println(" Hz (no samples)");
  return true;
}

void idotAudioOutputSetTone(bool on) {
  if (!gReady) {
    // setup() intentionally drives the logical buzzer OFF before audio bring-up;
    // suppress that harmless OFF request and report only a lost ON request.
    if (on) Serial.println("AUDIO: tone ON request ignored, backend not ready");
    return;
  }
  if (gToneOn == on) return;
  gToneOn = on;
  Serial.print("AUDIO: tone "); Serial.println(on ? "ON" : "OFF");
  if (!on) gPhase = 0;
}

bool idotAudioOutputReady() {
  return gReady;
}

#endif
