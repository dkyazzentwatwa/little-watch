#pragma once

// Little Cube OS feature flags (spec §3). RTC/IMU/battery hardware is verified
// present on this board; IMU stays off in v1 because its absolute axis signs
// proved unreliable on this module (magnitude-based gestures only, later).

#define FEATURE_TOUCH 1
#define FEATURE_MICROPHONE 1
#define FEATURE_SPEAKER 1
#define FEATURE_SD_CARD 1
#define FEATURE_USB_SERIAL 1
#define FEATURE_WIFI_PROVISIONING_AP 1

#define FEATURE_RTC 1
#define FEATURE_IMU 0
#define FEATURE_BATTERY_MONITOR 1
#define FEATURE_CAMERA 0

// Home screen layout (spec §7). Carousel is the default; the 2x2 grid is a
// compile-time alternative.
enum class HomeLayout {
  Carousel,
  Grid2x2,
};

#ifndef LITTLECUBE_HOME_LAYOUT
#define LITTLECUBE_HOME_LAYOUT HomeLayout::Carousel
#endif
