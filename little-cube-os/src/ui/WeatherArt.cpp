#include "WeatherArt.h"

#include <string.h>

namespace weatherart {

namespace {

enum Art : uint8_t {
  kClear,
  kPartly,
  kCloudy,
  kFog,
  kDrizzle,
  kRain,
  kSnow,
  kShowers,
  kSnowShowers,
  kThunder,
  kUnknown,
  kArtCount,
};

// Rows 0-2 carry the sky (sun disc or cloud), rows 3-4 the precipitation, so
// the shapes stay on a common baseline across conditions and the block does
// not appear to jump when the weather changes.
//
// Every line below is exactly kCols (9) characters. The static_assert block at
// the bottom of this file checks that mechanically rather than by counting in
// a review.
constexpr const char* const kArt[kArtCount][kRows] = {
    // Clear — the wttr.in sun: ".-." curves up because '.' inks low in the
    // cell and '-' inks the middle; "`-'" curves down because '`' and '\''
    // ink high.
    {"  \\   /  ",
     "   .-.   ",
     "- (   ) -",
     "   `-'   ",
     "  /   \\  "},
    // Mostly clear — sun behind a cloud. '"' is the sun's shoulder catching
    // the light; it inks rows 0-2, high in the cell, which is where that
    // highlight belongs.
    {"  \\  /   ",
     "_ /\"\".-. ",
     "  \\_(   )",
     "/(___(__)",
     "         "},
    // Overcast — cloud only.
    {"  .--.   ",
     ".-(    ).",
     "(___.__) ",
     "         ",
     "         "},
    // Fog — alternating bands, '_' on row 6 of its cell and '-' on row 3, so
    // the two interleave into a layered mist rather than a grid.
    {"         ",
     "_ - _ - _",
     " _ - _ - ",
     "_ - _ - _",
     " _ - _ - "},
    // Drizzle — the lightest precipitation mark in the set. '\'' inks rows
    // 0-3, so the drops hang from the top of their cells.
    {"  .--.   ",
     ".-(    ).",
     "(___.__) ",
     "  ' ' '  ",
     " ' ' '   "},
    // Rain — single streaks.
    {"  .--.   ",
     ".-(    ).",
     "(___.__) ",
     "  / / /  ",
     " / / /   "},
    // Snow — '*' is the densest small glyph in the font (rows 0-6, all five
    // columns), which is what makes a flake read as a flake at this size.
    {"  .--.   ",
     ".-(    ).",
     "(___.__) ",
     "  * * *  ",
     " * * *   "},
    // Showers — doubled streaks, heavier than Rain and offset between rows.
    {"  .--.   ",
     ".-(    ).",
     "(___.__) ",
     "// // // ",
     " // // //"},
    // Snow showers — flakes and streaks interleaved, so it reads as neither
    // pure snow nor pure rain.
    {"  .--.   ",
     ".-(    ).",
     "(___.__) ",
     " * / * / ",
     "* / * / *"},
    // Thunderstorm — two bolts. '_' (row 6) then '/' (rows 1-5) makes the
    // step, and the '/' on the row below continues the descent.
    {"  .--.   ",
     ".-(    ).",
     "(___.__) ",
     "  _/ /_  ",
     "  /   /  "},
    // Weather — the fallback, for a condition string this table does not
    // know. A '?' inside the cloud says "unknown", which is the honest
    // reading; silently drawing Clear would not be.
    {"  .--.   ",
     ".-(  ?  )",
     "(___.__) ",
     "         ",
     "         "},
};

// Three characters each, aviation-report flavoured. Fixed width is the point:
// a column of these cannot truncate the way the condition text did.
constexpr const char* const kCode[kArtCount] = {
    "CLR",  // Clear
    "FEW",  // Mostly clear
    "OVC",  // Overcast
    "FOG",  // Fog
    "DZL",  // Drizzle
    "RAI",  // Rain
    "SNW",  // Snow
    "SHR",  // Showers
    "SSH",  // Snow showers
    "TSM",  // Thunderstorm
    "WX?",  // Weather / unrecognised
};

Art idFor(const char* condition) {
  if (condition == nullptr || condition[0] == '\0') {
    return kUnknown;
  }
  if (strcmp(condition, "Clear") == 0) return kClear;
  if (strcmp(condition, "Mostly clear") == 0) return kPartly;
  if (strcmp(condition, "Overcast") == 0) return kCloudy;
  if (strcmp(condition, "Fog") == 0) return kFog;
  if (strcmp(condition, "Drizzle") == 0) return kDrizzle;
  if (strcmp(condition, "Rain") == 0) return kRain;
  if (strcmp(condition, "Snow") == 0) return kSnow;
  if (strcmp(condition, "Showers") == 0) return kShowers;
  if (strcmp(condition, "Snow showers") == 0) return kSnowShowers;
  if (strcmp(condition, "Thunderstorm") == 0) return kThunder;
  return kUnknown;  // includes the service's own "Weather"
}

// The grid contract, enforced at compile time over the WHOLE table rather
// than by counting spaces in a review. WeatherApp's measured extents assume
// every line is exactly kCols wide; a short or long line would silently move
// ink outside the envelope those numbers were checked against.
constexpr bool everyRowIsKColsWide() {
  for (int a = 0; a < kArtCount; ++a) {
    for (int r = 0; r < kRows; ++r) {
      int n = 0;
      while (kArt[a][r][n] != '\0') {
        ++n;
      }
      if (n != kCols) {
        return false;
      }
    }
  }
  return true;
}
static_assert(everyRowIsKColsWide(), "every art row must be exactly kCols characters");

constexpr bool everyCodeIsThreeWide() {
  for (int a = 0; a < kArtCount; ++a) {
    int n = 0;
    while (kCode[a][n] != '\0') {
      ++n;
    }
    if (n != 3) {
      return false;
    }
  }
  return true;
}
static_assert(everyCodeIsThreeWide(), "every condition code must be exactly 3 characters");

}  // namespace

const char* const* forCondition(const char* condition) {
  return kArt[idFor(condition)];
}

const char* codeForCondition(const char* condition) {
  return kCode[idFor(condition)];
}

}  // namespace weatherart
