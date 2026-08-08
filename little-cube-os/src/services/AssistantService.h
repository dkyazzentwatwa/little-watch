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

// Voice assistant (docs/superpowers/specs/2026-07-25-voice-assistant-design.md).
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

  bool askText(const char* question);        // serial: skip STT
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
  friend void assistantWorkerBody(AssistantService* self);

  enum class Mode : uint8_t { Voice, Text, TranscribeOnly };

  bool launchWorker(Mode mode);
  void realtimeWorkerBody();
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
  volatile uint8_t workerPhase_ = 0;  // mirrors Transcribing/Thinking for the UI
  volatile bool workerFailed_ = false;

  Mode mode_ = Mode::Voice;
  uint32_t listenStartMs_ = 0;
  bool stopRequested_ = false;

  char textQuery_[512] = "";
  char transcribePath_[128] = "";
  char transcript_[512] = "";  // worker-written before the done give
  char answer_[1024] = "";     // worker-written before the done give
  char lastError_[96] = "";    // worker-written before the done give

  // One PSRAM block, [exchange][user|assistant] slots of kHistorySlot chars.
  // Strings here held ~12 KB of internal heap hostage and fragmented it below
  // what a TLS handshake needs (field failure: voice follow-ups died at
  // "STT -1" with the largest free block at 31 KB and a full history).
  static constexpr size_t kHistorySlot = 1024;
  char* historyBuf_ = nullptr;
  uint8_t historyCount_ = 0;

  char* historySlot(uint8_t exchange, uint8_t side) const {
    return historyBuf_ + (static_cast<size_t>(exchange) * 2 + side) * kHistorySlot;
  }
};
