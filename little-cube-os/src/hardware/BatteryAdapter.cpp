#include "BatteryAdapter.h"

#include <Wire.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include "../board_config.h"

namespace {
XPowersPMU pmu;
}

bool BatteryAdapter::begin() {
  ready_ = pmu.begin(Wire, I2C_ADDR_PMU_AXP2101, PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  if (!ready_) {
    Serial.println("[battery] AXP2101 not found");
    return false;
  }
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  Serial.printf("[battery] AXP2101 ok id=0x%X\n", static_cast<unsigned>(pmu.getChipID()));
  return true;
}

int BatteryAdapter::percent() {
  if (!ready_ || !pmu.isBatteryConnect()) {
    return -1;
  }
  return pmu.getBatteryPercent();
}

bool BatteryAdapter::charging() {
  return ready_ && pmu.isCharging();
}

bool BatteryAdapter::vbusPresent() {
  return ready_ && pmu.isVbusIn();
}

bool BatteryAdapter::batteryPresent() {
  return ready_ && pmu.isBatteryConnect();
}
