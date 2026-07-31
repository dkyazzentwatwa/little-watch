#include "AssistantService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
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

// Shared HTTPS setup (podcast precedent): no on-device cert store; HTTP/1.0
// only when a raw body stream is read back (avoids chunked framing).
void beginHttps(HTTPClient& http, WiFiClientSecure& client, const char* url,
                const String& key, bool http10) {
  client.setInsecure();  // no cert store on-device; documented tradeoff
  if (http10) {
    http.useHTTP10(true);
  }
  http.begin(client, url);
  http.setConnectTimeout(10000);
  http.setTimeout(kHttpTimeoutMs);
  // The key travels only inside this header; it is never logged or echoed.
  http.addHeader("Authorization", String("Bearer ") + key);
}

// Copies a trimmed HTTP error body into out for diagnostics (safe: OpenAI
// error JSON, never the request headers).
void snipBody(HTTPClient& http, char* out, size_t cap) {
  String body = http.getString();
  body.replace('\n', ' ');
  strncpy(out, body.c_str(), cap - 1);
  out[cap - 1] = '\0';
}

}  // namespace

// ---------------------------------------------------------------------------
// Worker task: STT -> chat -> TTS. Serial prints are diagnostics only; all UI
// state flows through workerPhase_/done_ and the loop task's update().

// The exchange body lives in its own function so that returning from it
// unwinds the stack: vTaskDelete() never runs destructors, and leaking the
// TLS context + HTTP client every exchange starved later handshakes of
// internal RAM (69 KB free at boot -> 52 -> 49 across takes in the field,
// then esp-aes alloc failures).
void assistantWorkerBody(AssistantService* self) {
  self->workerFailed_ = false;
  const String key = self->settings_->openaiKey();

  // Let the idle task reap the just-deleted record task's stack (a 6 KB block
  // that would otherwise sit mid-heap through the TLS handshake).
  vTaskDelay(pdMS_TO_TICKS(20));
  Serial.printf("[assistant] exchange start — internal heap %u KB free (largest %u KB)\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  // ONE TLS connection for the whole exchange: api.openai.com honors
  // keep-alive, so STT -> chat -> TTS ride a single handshake. Opening three
  // separate ~50 KB mbedTLS sessions per exchange fragmented internal RAM
  // until esp-aes could not allocate (the field failures "STT -1"/"STT -3").
  WiFiClientSecure client;
  HTTPClient http;
  http.setReuse(true);

  // Phase 1: STT (skipped for text questions).
  if (self->mode_ != AssistantService::Mode::Text) {
    self->workerPhase_ = static_cast<uint8_t>(AssistantService::State::Transcribing);
    const char* wavPath = self->mode_ == AssistantService::Mode::TranscribeOnly
                              ? self->transcribePath_
                              : kQueryPath;
    fs::File f = SD_MMC.open(wavPath, FILE_READ);
    if (!f || f.size() <= 44) {
      snprintf(self->lastError_, sizeof(self->lastError_), "no captured audio");
      self->workerFailed_ = true;
      if (f) f.close();
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
        f.close();
      } else {
        memcpy(body, prefix.c_str(), prefix.length());
        size_t off = prefix.length();
        while (f.available() > 0) {
          const size_t n = f.read(body + off, 16384);
          if (n == 0) {
            break;
          }
          off += n;
        }
        f.close();
        memcpy(body + off, suffix.c_str(), suffix.length());
        off += suffix.length();

        beginHttps(http, client, "https://api.openai.com/v1/audio/transcriptions",
                   key, false);
        http.addHeader("Content-Type",
                       String("multipart/form-data; boundary=") + kBoundary);
        int code = http.POST(body, off);
        if (code < 0) {
          vTaskDelay(pdMS_TO_TICKS(750));  // transport hiccup: one retry
          code = http.POST(body, off);
        }
        heap_caps_free(body);
        if (code != 200) {
          char snip[48] = "";
          if (code > 0) snipBody(http, snip, sizeof(snip));
          snprintf(self->lastError_, sizeof(self->lastError_), "STT %d %s", code, snip);
          self->workerFailed_ = true;
        } else {
          // getString handles chunked HTTP/1.1 bodies; the reply is small.
          JsonDocument doc;
          if (deserializeJson(doc, http.getString()) != DeserializationError::Ok ||
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
      u["content"] = self->historySlot(i, 0);
      JsonObject a = msgs.add<JsonObject>();
      a["role"] = "assistant";
      a["content"] = self->historySlot(i, 1);
    }
    JsonObject cur = msgs.add<JsonObject>();
    cur["role"] = "user";
    cur["content"] = self->transcript_;
    String body;
    serializeJson(req, body);

    beginHttps(http, client, "https://api.openai.com/v1/chat/completions", key, false);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (code < 0) {
      vTaskDelay(pdMS_TO_TICKS(750));  // transport hiccup: one retry
      code = http.POST(body);
    }
    if (code != 200) {
      char snip[48] = "";
      if (code > 0) snipBody(http, snip, sizeof(snip));
      snprintf(self->lastError_, sizeof(self->lastError_), "chat %d %s", code, snip);
      self->workerFailed_ = true;
    } else {
      JsonDocument filter;
      filter["choices"][0]["message"]["content"] = true;
      JsonDocument doc;
      if (deserializeJson(doc, http.getString(),
                          DeserializationOption::Filter(filter)) !=
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

  // Phase 3: TTS -> reply.wav.partial -> finalize. Playback is started by the
  // loop task in update(), never here.
  if (!self->workerFailed_) {
    JsonDocument req;
    req["model"] = kTtsModel;
    req["voice"] = kTtsVoice;
    req["input"] = self->answer_;
    req["response_format"] = "wav";
    String body;
    serializeJson(req, body);

    // HTTP/1.0: the reply body is streamed raw off the socket (podcast rule).
    // Fine on the reused connection — this is the exchange's last call.
    beginHttps(http, client, "https://api.openai.com/v1/audio/speech", key, true);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (code < 0) {
      vTaskDelay(pdMS_TO_TICKS(750));  // transport hiccup: one retry
      code = http.POST(body);
    }
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
        int remaining = http.getSize();  // -1 = no Content-Length: read to close
        uint32_t idleMs = 0;
        uint32_t written = 0;
        while (http.connected() && (remaining > 0 || remaining == -1)) {
          const size_t avail = stream->available();
          if (avail == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            idleMs += 10;
            if (idleMs > kHttpTimeoutMs) {
              break;
            }
            continue;
          }
          idleMs = 0;
          const int n = stream->readBytes(
              buf, avail > sizeof(buf) ? sizeof(buf) : avail);
          if (n <= 0) {
            break;
          }
          if (out.write(buf, n) != static_cast<size_t>(n)) {
            snprintf(self->lastError_, sizeof(self->lastError_), "SD full");
            self->workerFailed_ = true;
            break;
          }
          written += n;
          if (remaining > 0) {
            remaining -= n;
          }
        }
        out.close();
        if (!self->workerFailed_ && written <= 44) {
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

}

void assistantWorkerTask(void* arg) {
  AssistantService* self = static_cast<AssistantService*>(arg);
  assistantWorkerBody(self);  // returning unwinds — every local is destroyed
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
  if (historyBuf_ == nullptr) {
    historyBuf_ = static_cast<char*>(
        heap_caps_malloc(static_cast<size_t>(kMaxExchanges) * 2 * kHistorySlot,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (historyBuf_ == nullptr) {
      Serial.println("[assistant] history buffer allocation failed; memoryless");
    }
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
    if (systemState_ != nullptr) systemState_->version++;
    state_ = State::Error;
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
  stopRequested_ = false;  // discard: back to Idle, nothing is sent
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
  if (card_ == nullptr || !card_->writable()) {
    snprintf(lastError_, sizeof(lastError_), "transcription needs the SD card");
    state_ = State::Error;
    if (systemState_ != nullptr) systemState_->version++;
    return false;
  }
  strncpy(transcribePath_, wavPath, sizeof(transcribePath_) - 1);
  transcribePath_[sizeof(transcribePath_) - 1] = '\0';
  return launchWorker(Mode::TranscribeOnly);
}

bool AssistantService::launchWorker(Mode mode) {
  // Every byte of contiguous internal heap matters during the TLS handshake,
  // and the I2S driver's DMA buffers sit exactly there. Recording is done and
  // reply playback cold-starts the driver again, so let it go for the network
  // phase (no-op if music is playing — the check keeps it honest).
  if (audio_ != nullptr) {
    audio_->releaseDriverIfIdle();
  }
  mode_ = mode;
  transcript_[0] = '\0';
  answer_[0] = '\0';
  lastError_[0] = '\0';
  workerPhase_ = static_cast<uint8_t>(State::Transcribing);
  xSemaphoreTake(done_, 0);      // drain a stale give before arming
  state_ = State::Transcribing;  // published BEFORE the task can observe it
  if (systemState_ != nullptr) systemState_->version++;
  if (xTaskCreate(assistantWorkerTask, "assistant", 20480, this, 1, nullptr) != pdPASS) {
    state_ = State::Error;
    snprintf(lastError_, sizeof(lastError_), "task start failed");
    return false;
  }
  return true;  // nothing is assigned after xTaskCreate
}

void AssistantService::pushHistory(const char* user, const char* assistant) {
  if (historyBuf_ == nullptr) {
    return;  // memoryless fallback: every question stands alone
  }
  if (historyCount_ == kMaxExchanges) {
    memmove(historyBuf_, historyBuf_ + 2 * kHistorySlot,
            static_cast<size_t>(kMaxExchanges - 1) * 2 * kHistorySlot);
    historyCount_--;
  }
  strncpy(historySlot(historyCount_, 0), user, kHistorySlot - 1);
  historySlot(historyCount_, 0)[kHistorySlot - 1] = '\0';
  strncpy(historySlot(historyCount_, 1), assistant, kHistorySlot - 1);
  historySlot(historyCount_, 1)[kHistorySlot - 1] = '\0';
  historyCount_++;
}

void AssistantService::resetHistory() {
  historyCount_ = 0;  // slots are overwritten by the next push
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
      if (phase != state_ &&
          (phase == State::Transcribing || phase == State::Thinking)) {
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
