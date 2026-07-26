# Voice Assistant (OpenAI) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record a spoken question on the cube, transcribe + answer it via the OpenAI API, and play the synthesized answer through the speaker, with short multi-turn memory.

**Architecture:** New `AssistantService` (state machine + one FreeRTOS worker per exchange, OpenAI HTTP client in its .cpp), new `AssistantApp` (talk button + status + transcript), new `assistant` serial family, key in NVS. Follows the podcast-download task pattern for HTTP and the AudioAdapter task-lifecycle contract for state.

**Tech Stack:** Arduino CLI only (esp32 core 3.3.8), HTTPClient + WiFiClientSecure (`setInsecure()`, repo-wide tradeoff), ArduinoJson, SD_MMC, ESP_I2S via the existing AudioAdapter.

**Plan adaptations (per CLAUDE.md, overriding skill defaults):** there is no unit-test suite or host harness — every task verifies with `./scripts/build.sh` (zero warnings tolerated as errors of taste) and on-device serial checks; a clean compile is never claimed as working. No git commits — the user keeps all work uncommitted in this tree.

Spec: `docs/superpowers/specs/2026-07-25-voice-assistant-design.md`

---

### Task 1: API key storage in SettingsService

**Files:**
- Modify: `little-cube-os/src/services/SettingsService.h` (public accessors + private field)
- Modify: `little-cube-os/src/services/SettingsService.cpp` (load in `begin()`, persist in setter)

- [ ] **Step 1: Header** — after the `setVolumePercent` block in `SettingsService.h` add:

```cpp
  // OpenAI API key for the Assistant (spec: voice-assistant design doc).
  // NVS only — never SD, never printed, never echoed over serial.
  String openaiKey() const { return openaiKey_; }
  void setOpenaiKey(const String& value);
  bool hasOpenaiKey() const { return openaiKey_.length() > 0; }
```

and a private `String openaiKey_;` next to the other String fields.

- [ ] **Step 2: Impl** — in `SettingsService.cpp`, add key constant next to the others (`constexpr const char* kKeyOpenaiKey = "aikey";`), load in `begin()` (`openaiKey_ = prefs.getString(kKeyOpenaiKey, "");`), and:

```cpp
void SettingsService::setOpenaiKey(const String& value) {
  openaiKey_ = value;
  prefs.putString(kKeyOpenaiKey, openaiKey_);
}
```

- [ ] **Step 3: Build** — `./scripts/build.sh` → clean compile.

### Task 2: AssistantService (state machine + OpenAI client)

**Files:**
- Create: `little-cube-os/src/services/AssistantService.h`
- Create: `little-cube-os/src/services/AssistantService.cpp`

- [ ] **Step 1: Header** — full contents:

```cpp
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class AudioAdapter;
class WifiService;
class SettingsService;
class SdStorage;
class SdCardAdapter;
struct SystemState;

// Voice assistant (spec: docs/superpowers/specs/2026-07-25-voice-assistant-design.md).
// Record a question -> OpenAI STT -> chat (short history) -> OpenAI TTS ->
// speaker. One FreeRTOS worker per exchange; the same task-lifecycle contract
// as AudioAdapter: state_ is written by the LOOP TASK ONLY, a worker's last
// two statements are xSemaphoreGive(done_) then vTaskDelete(nullptr), and a
// non-idle state is only left in update() by taking that semaphore.
class AssistantService {
 public:
  enum class State : uint8_t { Idle, Listening, Transcribing, Thinking, Speaking, Error };

  void begin(AudioAdapter* audio, WifiService* wifi, SettingsService* settings,
             SdStorage* storage, SdCardAdapter* card, SystemState* state);
  void update(uint32_t deltaMs);

  // Tap 1 / tap 2 on the talk button (also `assistant voice` over serial).
  bool startListening();
  bool finishListening();
  void cancelListening();  // Back while listening: drop the take, send nothing

  bool askText(const char* question);      // serial: skip STT
  bool transcribeOnly(const char* wavPath);  // serial: STT only, prints result
  void resetHistory();

  State state() const { return state_; }
  const char* stateName() const;
  const char* lastError() const { return lastError_; }
  const char* lastTranscript() const { return transcript_; }
  const char* lastAnswer() const { return answer_; }
  bool capturing() const { return state_ == State::Listening; }
  bool busy() const { return state_ != State::Idle && state_ != State::Error; }
  uint8_t historyDepth() const { return historyCount_; }
  uint32_t listenElapsedMs() const;

 private:
  friend void assistantWorkerTask(void* arg);

  enum class Mode : uint8_t { Voice, Text, TranscribeOnly };

  bool launchWorker(Mode mode);
  void pushHistory(const char* user, const char* assistant);

  static constexpr uint32_t kMaxListenMs = 30000;
  static constexpr uint8_t kMaxExchanges = 6;

  AudioAdapter* audio_ = nullptr;
  WifiService* wifi_ = nullptr;
  SettingsService* settings_ = nullptr;
  SdStorage* storage_ = nullptr;
  SdCardAdapter* card_ = nullptr;
  SystemState* systemState_ = nullptr;

  State state_ = State::Idle;  // loop task only
  SemaphoreHandle_t done_ = nullptr;
  volatile uint8_t workerPhase_ = 0;  // mirrors State::Transcribing/Thinking while running
  volatile bool workerFailed_ = false;

  Mode mode_ = Mode::Voice;
  uint32_t listenStartMs_ = 0;
  bool stopRequested_ = false;

  char textQuery_[512] = "";
  char transcribePath_[128] = "";
  char transcript_[512] = "";   // worker-written before the done give
  char answer_[1024] = "";      // worker-written before the done give
  char lastError_[96] = "";     // worker-written before the done give

  String historyUser_[kMaxExchanges];
  String historyAssistant_[kMaxExchanges];
  uint8_t historyCount_ = 0;
};
```

- [ ] **Step 2: Implementation** — full contents of `AssistantService.cpp`:

```cpp
#include "AssistantService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "../core/SystemState.h"
#include "../hardware/SdCardAdapter.h"
#include "../hardware/audio/AudioAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "SettingsService.h"
#include "WifiService.h"

namespace {

// All tunables in one place (spec "Data flow"). Changing a model is a rebuild.
constexpr const char* kSttModel = "gpt-4o-mini-transcribe";  // fallback: whisper-1
constexpr const char* kChatModel = "gpt-4o-mini";
constexpr const char* kTtsModel = "gpt-4o-mini-tts";
constexpr const char* kTtsVoice = "alloy";
constexpr const char* kSystemPrompt =
    "You are the voice assistant inside a tiny desk cube. Answer in one to "
    "three short spoken sentences. Plain text only: no markdown, no lists, "
    "no emoji.";
constexpr int kMaxAnswerTokens = 220;
constexpr uint32_t kHttpTimeoutMs = 30000;

constexpr const char* kAssistantDir = "/littlecube/assistant";
constexpr const char* kQueryPath = "/littlecube/assistant/query.wav";
constexpr const char* kReplyPath = "/littlecube/assistant/reply.wav";
constexpr const char* kBoundary = "----littlecube7f3a9c";

// Shared HTTPS setup (podcast precedent): no on-device cert store, HTTP/1.0
// when a raw body stream is read back.
void beginHttps(HTTPClient& http, WiFiClientSecure& client, const char* url,
                const String& key, bool http10) {
  client.setInsecure();  // no cert store on-device; documented tradeoff
  if (http10) {
    http.useHTTP10(true);
  }
  http.begin(client, url);
  http.setConnectTimeout(10000);
  http.setTimeout(kHttpTimeoutMs);
  http.addHeader("Authorization", String("Authorization: Bearer ").substring(0, 0) +
                                      "Bearer " + key);
}

// Copies at most cap-1 chars of an HTTP error body into out for diagnostics.
void snipBody(HTTPClient& http, char* out, size_t cap) {
  String body = http.getString();
  body.replace('\n', ' ');
  strncpy(out, body.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

}  // namespace

// ---------------------------------------------------------------------------

void assistantWorkerTask(void* arg) {
  AssistantService* self = static_cast<AssistantService*>(arg);
  self->workerFailed_ = false;
  const String key = self->settings_->openaiKey();

  // Phase 1: STT (voice modes only).
  if (self->mode_ != AssistantService::Mode::Text) {
    self->workerPhase_ = static_cast<uint8_t>(AssistantService::State::Transcribing);
    const char* wavPath = self->mode_ == AssistantService::Mode::TranscribeOnly
                              ? self->transcribePath_
                              : kQueryPath;
    fs::File f = SD_MMC.open(wavPath, FILE_READ);
    if (!f || f.size() <= 44) {
      snprintf(self->lastError_, sizeof(self->lastError_), "no captured audio");
      self->workerFailed_ = true;
    } else {
      // One PSRAM buffer holds the whole multipart body: prefix + WAV + suffix.
      const String prefix = String("--") + kBoundary +
                            "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" +
                            kSttModel + "\r\n--" + kBoundary +
                            "\r\nContent-Disposition: form-data; name=\"file\"; "
                            "filename=\"query.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
      const String suffix = String("\r\n--") + kBoundary + "--\r\n";
      const size_t total = prefix.length() + f.size() + suffix.length();
      uint8_t* body = static_cast<uint8_t*>(
          heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (body == nullptr) {
        snprintf(self->lastError_, sizeof(self->lastError_), "out of memory");
        self->workerFailed_ = true;
      } else {
        memcpy(body, prefix.c_str(), prefix.length());
        size_t off = prefix.length();
        while (f.available() > 0) {
          off += f.read(body + off, 16384);
        }
        memcpy(body + off, suffix.c_str(), suffix.length());
        off += suffix.length();

        WiFiClientSecure client;
        HTTPClient http;
        beginHttps(http, client, "https://api.openai.com/v1/audio/transcriptions",
                   key, false);
        http.addHeader("Content-Type",
                       String("multipart/form-data; boundary=") + kBoundary);
        const int code = http.POST(body, off);
        heap_caps_free(body);
        if (code != 200) {
          char snip[48] = "";
          if (code > 0) snipBody(http, snip, sizeof(snip));
          snprintf(self->lastError_, sizeof(self->lastError_), "STT %d %s", code, snip);
          self->workerFailed_ = true;
        } else {
          JsonDocument doc;
          if (deserializeJson(doc, http.getStream()) != DeserializationError::Ok ||
              !doc["text"].is<const char*>()) {
            snprintf(self->lastError_, sizeof(self->lastError_), "STT parse failed");
            self->workerFailed_ = true;
          } else {
            strncpy(self->transcript_, doc["text"], sizeof(self->transcript_) - 1);
            self->transcript_[sizeof(self->transcript_) - 1] = '\0';
          }
        }
        http.end();
      }
    }
    if (f) f.close();
  } else {
    strncpy(self->transcript_, self->textQuery_, sizeof(self->transcript_) - 1);
    self->transcript_[sizeof(self->transcript_) - 1] = '\0';
  }

  if (!self->workerFailed_ && self->transcript_[0] == '\0') {
    snprintf(self->lastError_, sizeof(self->lastError_), "didn't catch that");
    self->workerFailed_ = true;
  }
  if (self->mode_ == AssistantService::Mode::TranscribeOnly) {
    if (!self->workerFailed_) {
      Serial.printf("[assistant] transcript: %s\n", self->transcript_);
    }
    xSemaphoreGive(self->done_);
    vTaskDelete(nullptr);
    return;
  }

  // Phase 2: chat completion with short history.
  if (!self->workerFailed_) {
    self->workerPhase_ = static_cast<uint8_t>(AssistantService::State::Thinking);
    JsonDocument req;
    req["model"] = kChatModel;
    req["max_tokens"] = kMaxAnswerTokens;
    JsonArray msgs = req["messages"].to<JsonArray>();
    JsonObject sys = msgs.add<JsonObject>();
    sys["role"] = "system";
    sys["content"] = kSystemPrompt;
    for (uint8_t i = 0; i < self->historyCount_; i++) {
      JsonObject u = msgs.add<JsonObject>();
      u["role"] = "user";
      u["content"] = self->historyUser_[i];
      JsonObject a = msgs.add<JsonObject>();
      a["role"] = "assistant";
      a["content"] = self->historyAssistant_[i];
    }
    JsonObject cur = msgs.add<JsonObject>();
    cur["role"] = "user";
    cur["content"] = self->transcript_;
    String body;
    serializeJson(req, body);

    WiFiClientSecure client;
    HTTPClient http;
    beginHttps(http, client, "https://api.openai.com/v1/chat/completions", key, false);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(body);
    if (code != 200) {
      char snip[48] = "";
      if (code > 0) snipBody(http, snip, sizeof(snip));
      snprintf(self->lastError_, sizeof(self->lastError_), "chat %d %s", code, snip);
      self->workerFailed_ = true;
    } else {
      JsonDocument filter;
      filter["choices"][0]["message"]["content"] = true;
      JsonDocument doc;
      if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) !=
              DeserializationError::Ok ||
          !doc["choices"][0]["message"]["content"].is<const char*>()) {
        snprintf(self->lastError_, sizeof(self->lastError_), "chat parse failed");
        self->workerFailed_ = true;
      } else {
        strncpy(self->answer_, doc["choices"][0]["message"]["content"],
                sizeof(self->answer_) - 1);
        self->answer_[sizeof(self->answer_) - 1] = '\0';
      }
    }
    http.end();
  }

  // Phase 3: TTS -> /littlecube/assistant/reply.wav.partial -> finalize.
  if (!self->workerFailed_) {
    self->workerPhase_ = static_cast<uint8_t>(AssistantService::State::Thinking);
    JsonDocument req;
    req["model"] = kTtsModel;
    req["voice"] = kTtsVoice;
    req["input"] = self->answer_;
    req["response_format"] = "wav";
    String body;
    serializeJson(req, body);

    WiFiClientSecure client;
    HTTPClient http;
    // HTTP/1.0: the reply body is streamed raw off the socket (podcast rule).
    beginHttps(http, client, "https://api.openai.com/v1/audio/speech", key, true);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(body);
    if (code != 200) {
      char snip[48] = "";
      if (code > 0) snipBody(http, snip, sizeof(snip));
      snprintf(self->lastError_, sizeof(self->lastError_), "TTS %d %s", code, snip);
      self->workerFailed_ = true;
    } else {
      const String partial = AtomicFile::partialPath(kReplyPath);
      SD_MMC.remove(partial);
      fs::File out = SD_MMC.open(partial, FILE_WRITE);
      if (!out) {
        snprintf(self->lastError_, sizeof(self->lastError_), "SD write failed");
        self->workerFailed_ = true;
      } else {
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[2048];
        int remaining = http.getSize();  // -1 with no Content-Length: read to close
        uint32_t idleMs = 0;
        while (http.connected() && (remaining > 0 || remaining == -1)) {
          const size_t avail = stream->available();
          if (avail == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            idleMs += 10;
            if (idleMs > kHttpTimeoutMs) break;
            continue;
          }
          idleMs = 0;
          const int n = stream->readBytes(
              buf, avail > sizeof(buf) ? sizeof(buf) : avail);
          if (n <= 0) break;
          if (out.write(buf, n) != static_cast<size_t>(n)) {
            snprintf(self->lastError_, sizeof(self->lastError_), "SD full");
            self->workerFailed_ = true;
            break;
          }
          if (remaining > 0) remaining -= n;
        }
        out.close();
        if (!self->workerFailed_ && SD_MMC.open(partial, FILE_READ).size() <= 44) {
          snprintf(self->lastError_, sizeof(self->lastError_), "TTS empty reply");
          self->workerFailed_ = true;
        }
        if (!self->workerFailed_) {
          if (!AtomicFile::finalizePartial(SD_MMC, kReplyPath)) {
            snprintf(self->lastError_, sizeof(self->lastError_), "SD finalize failed");
            self->workerFailed_ = true;
          }
        } else {
          SD_MMC.remove(partial);
        }
      }
    }
    http.end();
  }

  xSemaphoreGive(self->done_);  // release barrier — nothing may touch self after
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------

void AssistantService::begin(AudioAdapter* audio, WifiService* wifi,
                             SettingsService* settings, SdStorage* storage,
                             SdCardAdapter* card, SystemState* state) {
  audio_ = audio;
  wifi_ = wifi;
  settings_ = settings;
  storage_ = storage;
  card_ = card;
  systemState_ = state;
  if (done_ == nullptr) {
    done_ = xSemaphoreCreateBinary();
  }
}

const char* AssistantService::stateName() const {
  switch (state_) {
    case State::Idle: return "idle";
    case State::Listening: return "listening";
    case State::Transcribing: return "transcribing";
    case State::Thinking: return "thinking";
    case State::Speaking: return "speaking";
    case State::Error: return "error";
  }
  return "?";
}

uint32_t AssistantService::listenElapsedMs() const {
  return state_ == State::Listening ? millis() - listenStartMs_ : 0;
}

bool AssistantService::startListening() {
  if (busy() || audio_ == nullptr || !audio_->recordIdle() || !audio_->playbackIdle()) {
    return false;  // half-duplex, and never two exchanges at once
  }
  if (card_ == nullptr || !card_->writable()) {
    snprintf(lastError_, sizeof(lastError_), "assistant needs the SD card");
    state_ = State::Error;
    if (systemState_ != nullptr) systemState_->version++;
    return false;
  }
  if (settings_ == nullptr || !settings_->hasOpenaiKey()) {
    snprintf(lastError_, sizeof(lastError_), "no API key — run: assistant key");
    state_ = State::Error;
    if (systemState_ != nullptr) systemState_->version++;
    return false;
  }
  if (systemState_ == nullptr || !systemState_->internet) {
    snprintf(lastError_, sizeof(lastError_), "offline — connect Wi-Fi first");
    state_ = State::Error;
    if (systemState_ != nullptr) systemState_->version++;
    return false;
  }
  storage_->makeDir(kAssistantDir);  // idempotent
  SD_MMC.remove(kQueryPath);
  if (!audio_->startRecordWav(kQueryPath, 16000)) {
    snprintf(lastError_, sizeof(lastError_), "mic start failed");
    state_ = State::Error;
    systemState_->version++;
    return false;
  }
  listenStartMs_ = millis();
  stopRequested_ = false;
  state_ = State::Listening;
  systemState_->version++;
  return true;
}

bool AssistantService::finishListening() {
  if (state_ != State::Listening) {
    return false;
  }
  stopRequested_ = true;  // update() launches the worker once the file closed
  audio_->requestStopRecord();
  return true;
}

void AssistantService::cancelListening() {
  if (state_ != State::Listening) {
    return;
  }
  stopRequested_ = false;  // discard: update() returns to Idle, sends nothing
  audio_->requestStopRecord();
  state_ = State::Idle;
  systemState_->version++;
}

bool AssistantService::askText(const char* question) {
  if (busy() || question == nullptr || question[0] == '\0') {
    return false;
  }
  if (settings_ == nullptr || !settings_->hasOpenaiKey()) {
    snprintf(lastError_, sizeof(lastError_), "no API key — run: assistant key");
    state_ = State::Error;
    if (systemState_ != nullptr) systemState_->version++;
    return false;
  }
  strncpy(textQuery_, question, sizeof(textQuery_) - 1);
  textQuery_[sizeof(textQuery_) - 1] = '\0';
  return launchWorker(Mode::Text);
}

bool AssistantService::transcribeOnly(const char* wavPath) {
  if (busy() || wavPath == nullptr) {
    return false;
  }
  strncpy(transcribePath_, wavPath, sizeof(transcribePath_) - 1);
  transcribePath_[sizeof(transcribePath_) - 1] = '\0';
  return launchWorker(Mode::TranscribeOnly);
}

bool AssistantService::launchWorker(Mode mode) {
  mode_ = mode;
  transcript_[0] = '\0';
  answer_[0] = '\0';
  lastError_[0] = '\0';
  workerPhase_ = static_cast<uint8_t>(State::Transcribing);
  xSemaphoreTake(done_, 0);  // drain a stale give before arming
  state_ = State::Transcribing;  // published BEFORE the task can observe it
  if (systemState_ != nullptr) systemState_->version++;
  if (xTaskCreate(assistantWorkerTask, "assistant", 20480, this, 1, nullptr) != pdPASS) {
    state_ = State::Error;
    snprintf(lastError_, sizeof(lastError_), "task start failed");
    return false;
  }
  return true;
}

void AssistantService::pushHistory(const char* user, const char* assistant) {
  if (historyCount_ == kMaxExchanges) {
    for (uint8_t i = 1; i < kMaxExchanges; i++) {
      historyUser_[i - 1] = historyUser_[i];
      historyAssistant_[i - 1] = historyAssistant_[i];
    }
    historyCount_--;
  }
  historyUser_[historyCount_] = String(user).substring(0, 1000);
  historyAssistant_[historyCount_] = String(assistant).substring(0, 1000);
  historyCount_++;
}

void AssistantService::resetHistory() {
  for (uint8_t i = 0; i < kMaxExchanges; i++) {
    historyUser_[i] = "";
    historyAssistant_[i] = "";
  }
  historyCount_ = 0;
}

void AssistantService::update(uint32_t /*deltaMs*/) {
  switch (state_) {
    case State::Listening:
      if (!stopRequested_ && listenElapsedMs() >= kMaxListenMs) {
        finishListening();  // auto-stop long takes
      }
      // The record task saves + gates + normalizes, then goes idle; only then
      // is query.wav complete on the card.
      if (stopRequested_ && audio_->recordIdle()) {
        stopRequested_ = false;
        if (audio_->recordingFailed() || audio_->recordedBytes() == 0) {
          snprintf(lastError_, sizeof(lastError_), "capture failed");
          state_ = State::Error;
          systemState_->version++;
        } else {
          launchWorker(Mode::Voice);
        }
      }
      break;
    case State::Transcribing:
    case State::Thinking: {
      // Mirror worker progress for the UI without the worker touching state_.
      const State phase = static_cast<State>(workerPhase_);
      if (phase != state_ && (phase == State::Transcribing || phase == State::Thinking)) {
        state_ = phase;
        systemState_->version++;
      }
      if (xSemaphoreTake(done_, 0) == pdTRUE) {
        if (workerFailed_) {
          state_ = State::Error;
          Serial.printf("[assistant] error: %s\n", lastError_);
        } else if (mode_ == Mode::TranscribeOnly) {
          state_ = State::Idle;
        } else {
          pushHistory(transcript_, answer_);
          // Playback starts from the loop task, honoring half-duplex.
          if (audio_->requestPlayWavFile(kReplyPath)) {
            state_ = State::Speaking;
          } else {
            snprintf(lastError_, sizeof(lastError_), "playback failed");
            state_ = State::Error;
          }
        }
        systemState_->version++;
      }
      break;
    }
    case State::Speaking:
      if (audio_->playbackIdle()) {
        state_ = State::Idle;
        systemState_->version++;
      }
      break;
    case State::Idle:
    case State::Error:
      break;
  }
}
```

- [ ] **Step 3: Build** — `./scripts/build.sh`. Fix compile errors only; no behavior tweaks.

### Task 3: Kernel wiring

**Files:**
- Modify: `little-cube-os/src/core/Services.h` (forward decl + pointer `AssistantService* assistant = nullptr;` next to `recorder`)
- Modify: `little-cube-os/src/core/Kernel.cpp`

- [ ] **Step 1** — Kernel.cpp: include `../services/AssistantService.h`, add file-scope `AssistantService assistantService;`, wire `services.assistant = &assistantService;`, call `assistantService.begin(&audioAdapter, &wifiService, &settingsService, &sdStorage, &sdCardAdapter, &systemState);` right after `recorderService.begin(...)`.

- [ ] **Step 2** — loop: add `assistantService.update(deltaMs);` immediately after `recorderService.update(deltaMs);` (reaper ordering comment applies: audio reaps first). Extend the capture-quiet condition — in `updateRecordingQuietCapture()` replace `const bool recording = recorderService.recording();` with:

```cpp
  const bool recording = recorderService.recording() || assistantService.capturing();
```

and the SD gate `if (!recorderService.recording())` with
`if (!recorderService.recording() && !assistantService.capturing())`.

- [ ] **Step 3: Build** — `./scripts/build.sh`.

### Task 4: AssistantApp + registration

**Files:**
- Modify: `little-cube-os/src/core/App.h` (`Assistant,` after `News,`; `kAppCount = 15`)
- Modify: `little-cube-os/src/core/AppRouter.cpp` (`appName()` case returning `"Assistant"`)
- Modify: `little-cube-os/src/apps/AppRegistry.cpp` (include, static instance, register)
- Create: `little-cube-os/src/apps/AssistantApp.h`
- Create: `little-cube-os/src/apps/AssistantApp.cpp`

- [ ] **Step 1: Header**:

```cpp
#pragma once

#include "../core/App.h"
#include "../core/Services.h"
#include "../ui/StatusBar.h"

// Voice assistant UI (spec: voice-assistant design doc): one big talk button
// (tap = listen, tap again = send), live pipeline status, and the last
// exchange's transcript + answer. All work happens in AssistantService; this
// app only reflects its state and forwards taps.
class AssistantApp : public App {
 public:
  explicit AssistantApp(Services& services) : services_(services) {}

  void onOpen() override;
  void onClose() override;
  void onPause() override {}
  void onResume() override { dirty_ = true; }

  void update(uint32_t deltaMs) override;
  void render() override;
  bool handleInput(const InputEvent& event) override;

 private:
  struct Rect {
    int16_t x = 0, y = 0, w = 0, h = 0;
    bool contains(int16_t px, int16_t py) const {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  Services& services_;
  StatusBar statusBar_;
  Rect talkRect_;
  uint8_t lastState_ = 255;
  uint32_t lastStateVersion_ = 0;
  uint32_t tickMs_ = 0;
  bool dirty_ = true;
};
```

- [ ] **Step 2: Implementation** (`AssistantApp.cpp`) — mirror RecorderApp's structure: `onOpen()` marks dirty; `onClose()` calls `services_.assistant->resetHistory();` (spec: memory cleared on app exit); `update()` marks dirty when `state()` changed or, while `Listening`, every 500 ms for the elapsed readout; `render()` draws StatusBar, a centered rounded talk button (`talkRect_`, label by state: "TALK" idle, "SEND" listening with elapsed seconds, "…" while transcribing/thinking, "SPEAKING", "TALK" again on error), the transcript (`lastTranscript()`, dim) and answer (`lastAnswer()`) word-wrapped below, and `lastError()` in `theme::kBad` when state is Error. `handleInput()`: Tap in `talkRect_` → `startListening()` / `finishListening()` by state; `Back` while listening → `cancelListening()` (consume); anything else falls through. Use only `theme::` constants and `services_.amoled->shiftX()/shiftY()` offsets like every other app.

- [ ] **Step 3** — App.h enum + count, `appName()` case, AppRegistry (`static AssistantApp assistant(services); router.registerApp(AppId::Assistant, &assistant);`).

- [ ] **Step 4: Build** — `./scripts/build.sh`.

### Task 5: `assistant` serial family + masked key prompt

**Files:**
- Create: `little-cube-os/src/serial/commands/AssistantCommands.h`
- Create: `little-cube-os/src/serial/commands/AssistantCommands.cpp`
- Modify: `little-cube-os/src/serial/SerialCommandService.h` (`PasswordPrompt assistantKeyPrompt_;`)
- Modify: `little-cube-os/src/serial/SerialCommandService.cpp` (prompt consumption + dispatch + help)

- [ ] **Step 1: Header**:

```cpp
#pragma once

#include "WifiCommands.h"  // PasswordPrompt

struct Services;

bool handleAssistantCommand(Services& services, PasswordPrompt& keyPrompt,
                            const char* verb, char* args);
void printAssistantHelp();
```

- [ ] **Step 2: Implementation** — verbs (`key` arms the prompt and prints "paste the OpenAI API key, or a blank line to cancel:"; `status` prints state, key set/missing — never the key —, online flag, history depth, last error; `ask` takes `cmdargs::rest` and calls `askText`; `transcribe` resolves a path via `services.storage->sanitizePath` and calls `transcribeOnly`; `voice` toggles `startListening()`/`finishListening()`; `reset` clears history). Follow RecordingsCommands' shape exactly.

- [ ] **Step 3: Prompt consumption** — in `SerialCommandService::update()`'s line handler, next to the `wifiPrompt_` branch, add an `assistantKeyPrompt_.active` branch: blank line cancels; otherwise `services_->settings->setOpenaiKey(line_)`, print "key stored", then `memset(line_, 0, sizeof(line_))` (the wifi branch's zeroing pattern — the key must not linger in the line buffer). Dispatch: `assistant` family wired like `recordings`; help gets one line ("assistant                     voice AI (assistant help)").

- [ ] **Step 4: Build** — `./scripts/build.sh`.

### Task 6: On-device verification (hardware rule: nothing is "working" until this passes)

- [ ] Flash: `./scripts/upload.sh`; confirm `boot: ready`.
- [ ] User enters key: they run `assistant key` in their own console and paste the key (never through the agent, never in chat).
- [ ] `assistant status` → `state: idle, key: set, online: yes, history: 0`.
- [ ] `assistant ask "Say the word hello"` → serial shows thinking → speaking; `files list /littlecube/assistant` shows `reply.wav` > 44 bytes; speaker plays audibly (user confirms).
- [ ] `assistant transcribe /littlecube/recordings/<existing voice note>` → transcript printed and plausible. If STT returns 404 for `gpt-4o-mini-transcribe`, switch `kSttModel` to `"whisper-1"`, rebuild, re-verify.
- [ ] User: open Assistant app, full tap-talk-listen loop; follow-up question referencing the first (history works); Back cancels a listen without sending.
- [ ] Add new unchecked items to `docs/hardware-validation.md` under a "## Assistant" section; check only what was exercised above.

### Self-review notes (done at write time)

Spec coverage: storage/key (T1), pipeline+states+history (T2), quieting+wiring (T3), UI+app registration (T4), serial+masked key (T5), testing (T6) — all spec sections mapped. `beginHttps` builds the Authorization header awkwardly in the draft (`String("Authorization: Bearer ").substring(0,0)+...`); implement as plain `http.addHeader("Authorization", String("Bearer ") + key);`. Types cross-checked: `AudioAdapter::recordIdle/playbackIdle/requestStopRecord/requestPlayWavFile/startRecordWav`, `SdStorage::makeDir/sanitizePath`, `AtomicFile::partialPath/finalizePartial`, `SystemState::internet/version` all exist in the current tree.
