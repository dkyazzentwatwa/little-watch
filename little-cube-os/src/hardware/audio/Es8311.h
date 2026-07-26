#pragma once

// Minimal self-contained ES8311 codec control over I2C (Wire).
//
// Ported from Espressif's esp-adf ES8311 driver
// (components/audio_hal/driver/es8311) — same register map and clock
// coefficient table — but stripped of the esp-adf i2c_bus/board abstractions so
// it talks directly to an Arduino TwoWire bus.
//
// Assumes the standard board wiring for this AMOLED module: MCLK is supplied by
// the ESP32 I2S peripheral (FROM_MCLK_PIN), MCLK = 256 * sampleRate, codec in
// I2S slave mode, 16-bit samples, onboard analog microphone (not DMIC).

#include <Arduino.h>
#include <Wire.h>

namespace Es8311 {

// Probes and initializes the codec for full-duplex 16-bit audio at sampleRate.
// Returns false if the chip does not ACK on I2C (e.g. wrong board).
bool init(TwoWire& wire, uint8_t addr, uint32_t sampleRate);

// The clean maximum: at and below this percentage the DAC runs at or under
// 0 dB. Above it the extra gain is digital and clips hot content — the Sound
// screen marks this point so the boost is an informed choice.
constexpr uint8_t kUnityVolumePercent = 80;

// DAC (speaker) volume, 0..100, mapped piecewise onto REG32 (0.5 dB/step):
// 0 = mute, 1..80% spans -40..0 dB, 81..100% spans 0..+10 dB.
void setVolume(uint8_t percent);

// ADC (microphone) gain, 0..7 = 0..42 dB in 6 dB steps (ES8311 REG16).
// Remembered and re-applied by init(): every recording cold-starts the codec,
// so a bare register write would not survive to the next take.
void setMicGain(uint8_t gain);
uint8_t micGain();

void mute(bool muted);

}  // namespace Es8311
