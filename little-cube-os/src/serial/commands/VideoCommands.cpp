#include "VideoCommands.h"

#if FEATURE_VIDEO

#include <Arduino.h>
#include <string.h>

#include "../../core/AppRouter.h"
#include "../../core/Services.h"
#include "../../services/VideoService.h"
#include "../../storage/SdStorage.h"
#include "../../storage/StoragePaths.h"
#include "../../video/VideoPlayer.h"
#include "../CmdArgs.h"

namespace {

void fmtMs(uint32_t ms, char* out, size_t len) {
  const uint32_t s = ms / 1000;
  snprintf(out, len, "%lu:%02lu", static_cast<unsigned long>(s / 60),
           static_cast<unsigned long>(s % 60));
}

// "mm:ss", "+N", "-N" (seconds) -> relative delta against `fromMs`.
bool parseSeek(const char* tok, uint32_t fromMs, int32_t& deltaOut) {
  if (tok == nullptr || tok[0] == '\0') {
    return false;
  }
  if (tok[0] == '+' || tok[0] == '-') {
    deltaOut = static_cast<int32_t>(strtol(tok, nullptr, 10)) * 1000;
    return true;
  }
  const char* colon = strchr(tok, ':');
  if (colon == nullptr) {
    return false;
  }
  const long m = strtol(tok, nullptr, 10);
  const long s = strtol(colon + 1, nullptr, 10);
  if (m < 0 || s < 0 || s >= 60) {
    return false;
  }
  deltaOut = static_cast<int32_t>((m * 60 + s) * 1000) - static_cast<int32_t>(fromMs);
  return true;
}

}  // namespace

bool handleVideoCommand(Services& services, const char* verb, char* args) {
  VideoService* video = services.video;
  VideoPlayer* player = services.videoPlayer;
  if (video == nullptr || player == nullptr) {
    Serial.println("video: unavailable (FEATURE_VIDEO off)");
    return true;
  }

  if (strcmp(verb, "list") == 0) {
    char* cursor = args;
    const char* dir = cmdargs::nextToken(cursor);
    // The default (paths::kVideo, when no argument is given) is already a
    // trusted constant and needs no sanitizing; an explicit argument is
    // arbitrary serial input like any other path and goes through the same
    // gate the app and every other command family use.
    String safeDir;
    const char* useDir = paths::kVideo;
    if (dir != nullptr) {
      if (services.storage == nullptr || !services.storage->sanitizePath(dir, safeDir)) {
        Serial.println("video: path refused");
        return true;
      }
      useDir = safeDir.c_str();
    }
    VideoInfo items[VideoService::kMaxListWindow];
    size_t total = 0;
    const size_t n = video->list(useDir, items, VideoService::kMaxListWindow, &total);
    for (size_t i = 0; i < n; i++) {
      if (items[i].isDir) {
        Serial.printf("  [dir]  %s\n", items[i].name);
      } else {
        char d[16];
        fmtMs(items[i].durationMs, d, sizeof(d));
        Serial.printf("  %-8s %s\n", d, items[i].name);
      }
    }
    Serial.printf("%u item(s)%s\n", static_cast<unsigned>(total),
                  total > n ? " (first page shown)" : "");
    return true;
  }
  if (strcmp(verb, "play") == 0) {
    char* cursor = args;
    const char* path = cmdargs::rest(cursor);
    if (path == nullptr) {
      Serial.println("usage: video play <path.lcv>");
      return true;
    }
    char full[160];
    if (path[0] == '/') {
      strncpy(full, path, sizeof(full) - 1);
      full[sizeof(full) - 1] = '\0';
    } else {
      snprintf(full, sizeof(full), "%s/%s", paths::kVideo, path);
    }
    // Sanitize once, up front, and use the SAME sanitized string for both
    // resumeMs() and play() below — resumeMs() has no sanitizing of its own,
    // and keying it off an un-sanitized path while play() (which
    // re-sanitizes internally — harmless) uses the sanitized one would let
    // the two disagree on which file a resume record belongs to.
    String safe;
    if (services.storage == nullptr || !services.storage->sanitizePath(full, safe)) {
      Serial.println("video: path refused");
      return true;
    }
    strncpy(full, safe.c_str(), sizeof(full) - 1);
    full[sizeof(full) - 1] = '\0';
    char why[48];
    if (!player->play(full, video->resumeMs(full), why, sizeof(why))) {
      Serial.printf("video: refused — %s\n", why);
      return true;
    }
    // Opening the app is the one UI hook a command gets (commands act
    // through services): VideoApp::onOpen adopts an already-running
    // playback and lands directly on the player screen, which is where
    // setUiActive happens. Without the app open, playback is headless —
    // audio and position run; frames are dropped on the clock.
    if (services.router != nullptr) {
      services.router->open(AppId::Video);
    }
    Serial.printf("video: playing %s\n", full);
    return true;
  }
  if (strcmp(verb, "pause") == 0) {
    if (player->idle()) {
      Serial.println("video: nothing playing");
      return true;
    }
    player->setPaused(true);
    Serial.println("video: paused");
    return true;
  }
  if (strcmp(verb, "resume") == 0) {
    if (player->idle()) {
      Serial.println("video: nothing playing");
      return true;
    }
    player->setPaused(false);
    Serial.println("video: resumed");
    return true;
  }
  if (strcmp(verb, "seek") == 0) {
    if (player->idle()) {
      Serial.println("video: nothing playing");
      return true;
    }
    char* cursor = args;
    const char* tok = cmdargs::nextToken(cursor);
    int32_t delta = 0;
    if (!parseSeek(tok, player->positionMs(), delta)) {
      Serial.println("usage: video seek <mm:ss | +sec | -sec>");
      return true;
    }
    player->requestSeek(delta);
    Serial.println("video: seeking");
    return true;
  }
  if (strcmp(verb, "stop") == 0) {
    if (player->idle()) {
      Serial.println("video: nothing playing");
      return true;
    }
    player->requestStop();
    Serial.println("video: stopping");
    return true;
  }
  if (strcmp(verb, "status") == 0) {
    if (player->idle()) {
      Serial.println("video: idle");
      return true;
    }
    char pos[16];
    char dur[16];
    fmtMs(player->positionMs(), pos, sizeof(pos));
    fmtMs(player->durationMs(), dur, sizeof(dur));
    // Orientation reads as "how the file is stored -> how it is being shown".
    // ROTATING marks the compatibility path (the two disagree, so every frame
    // is turned and scaled during decode) — the first thing to check if
    // playback is dropping frames.
    const VideoPlayer::PictureLayout& layout = player->pictureLayout();
    Serial.printf(
        "video: %s %s/%s %ux%u@%ufps %s->%s %ux%u@(%d,%d)%s shown=%lu dropped=%lu ring=%u%s\n",
        player->path(), pos, dur, player->header().width, player->header().height,
        player->header().fps,
        player->header().orientation == VideoOrientation::Upright ? "upright" : "rotated",
        player->activeOrientation() == VideoOrientation::Upright ? "upright" : "rotated",
        layout.w, layout.h, layout.x, layout.y, layout.rotate ? " ROTATING" : "",
        static_cast<unsigned long>(player->framesShown()),
        static_cast<unsigned long>(player->framesDropped()), player->ringDepth(),
        player->paused() ? " [paused]" : "");
    return true;
  }
  if (strcmp(verb, "queue") == 0) {
    if (player->idle()) {
      Serial.println("video: nothing playing");
      return true;
    }
    char next[160];
    if (services.video->nextInFolder(player->path(), next, sizeof(next))) {
      Serial.printf("video: next up %s\n", next);
    } else {
      Serial.println("video: last episode in its folder");
    }
    return true;
  }
  return false;
}

void printVideoHelp() {
  Serial.println("video list [dir]            list /littlecube/video (or a subfolder)");
  Serial.println("video play <path.lcv>       play (relative paths resolve under video/)");
  Serial.println("video pause | resume        pause / resume playback");
  Serial.println("video seek <mm:ss|+s|-s>    absolute or relative seek");
  Serial.println("video stop                  stop (position is saved by the app)");
  Serial.println("video status                path, position, fps, drops, ring depth");
  Serial.println("video queue                 show the next episode in the folder");
  Serial.println("Pack episodes on a computer: ./scripts/pack-video.sh input.mkv");
}

#endif  // FEATURE_VIDEO
