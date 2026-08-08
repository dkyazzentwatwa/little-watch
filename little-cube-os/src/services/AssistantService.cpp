#include "AssistantService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
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
constexpr const char* kTtsModel = "gpt-4o-mini-tts";
constexpr const char* kTtsVoice = "alloy";
constexpr const char* kRealtimeModel = "gpt-realtime-2.1";
constexpr const char* kRealtimeHost = "api.openai.com";
constexpr uint16_t kRealtimePort = 443;
// A TLS WebSocket connection can take longer than a regular API request after
// Wi-Fi wakes or roams.  More importantly, wait for the server's
// `session.created` event before configuring it: a completed WebSocket
// handshake alone does not mean the Realtime session is ready for events.
constexpr uint32_t kRealtimeConnectTimeoutMs = 30000;
constexpr uint32_t kRealtimeResponseTimeoutMs = 90000;
constexpr uint32_t kRealtimePcmRate = 24000;

// Responses API (replaces Chat Completions). UNVERIFIED against a live spec —
// these are constants precisely so a rejected request is a one-line fix and a
// reflash rather than a code change.
constexpr const char* kResponseModel = "gpt-5.6-luna";
// Empty string omits the reasoning object entirely. "none" may not be a valid
// effort for every model; if the API 400s on it, blank this.
constexpr const char* kReasoningEffort = "none";
// false omits the tools array entirely, reverting to a plain completion.
constexpr bool kWebSearchEnabled = true;
// The built-in tool's type string. Earlier previews spelled this
// "web_search_preview"; if the API rejects it, try that.
constexpr const char* kWebSearchTool = "web_search";
// The Responses API stores responses SERVER-SIDE BY DEFAULT. This is an
// always-on device in someone's home that hears whatever is said near it, so
// the default is wrong for it: every question and answer would be retained by
// OpenAI. Sent explicitly rather than relied upon — a default that silently
// flips is not something a privacy decision should rest on.
//
// This also means previous_response_id chaining is unavailable, which costs
// nothing here: conversation history is replayed from the PSRAM slots in
// `input` on every request, and it lives only while the app is open.
constexpr bool kStoreResponses = false;

// Sent as the request's `instructions` field: on the Responses API this
// replaces the old system-role message, so no system entry goes in `input`.
// Pure ASCII (it rides inside JSON), and the closing paragraph is the old
// system prompt's rule — this is a TTS pipeline and markdown gets read aloud.
constexpr const char* kInstructions =
    "You are a concise voice assistant running on an ESP32-based device.\n"
    "\n"
    "Use web search only when the answer depends on current, changing, or "
    "externally verifiable information, including: weather; news and recent "
    "events; prices and product availability; schedules and event times; "
    "sports scores, standings, and fixtures; current laws, rules, or "
    "regulations; current company, product, or software information; travel "
    "conditions; anything the user explicitly asks you to search or verify.\n"
    "\n"
    "Do not use web search for: casual conversation; jokes; timeless factual "
    "questions; basic explanations; writing or rewriting; calculations; "
    "device commands; questions that can be answered reliably from general "
    "knowledge.\n"
    "\n"
    "Keep spoken responses concise and natural. Prefer one to three short "
    "sentences unless the user asks for more detail. Do not read URLs or "
    "citation metadata aloud.\n"
    "\n"
    "Plain text only: no markdown, no lists, no emoji.";

constexpr int kMaxAnswerTokens = 220;
constexpr uint32_t kHttpTimeoutMs = 30000;
// Phase 2 only. A web-search-backed answer runs tool calls server-side before
// the first byte arrives, which routinely outlasts a plain completion. STT and
// TTS keep kHttpTimeoutMs: raising it globally would let a wedged upload or a
// stalled audio stream hold the worker (and the UI's Thinking state) twice as
// long for no gain. HTTPClient::setTimeout takes a uint16_t -> 65535 ms ceiling.
constexpr uint32_t kResponsesTimeoutMs = 60000;

// Phase 2's response body is sunk into this much PSRAM. Deliberately generous:
// a plain completion is a couple of KB, but a web-search-backed answer carries
// search calls, queries and citation blocks and runs far larger. PSRAM is 8 MB,
// so the headroom is free — whereas internal RAM, which is what the old
// getString() was consuming, is the thing that actually runs out on this board
// (see the TLS fragmentation note above assistantWorkerBody).
constexpr size_t kResponseBufferBytes = 64 * 1024;

constexpr const char* kAssistantDir = "/littlecube/assistant";
constexpr const char* kQueryPath = "/littlecube/assistant/query.wav";
constexpr const char* kReplyPath = "/littlecube/assistant/reply.wav";
constexpr const char* kBoundary = "----littlecube7f3a9c";

// A compact encoder/decoder keeps the realtime path independent from the
// WebSockets library's protected helper and lets chunks stay in PSRAM. Audio
// payloads use only this alphabet, so no JSON escaping is required.
constexpr char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64Encode(const uint8_t* input, size_t inputBytes, char* output) {
  size_t src = 0;
  size_t dst = 0;
  while (src < inputBytes) {
    const uint32_t a = input[src++];
    const bool hasB = src < inputBytes;
    const uint32_t b = hasB ? input[src++] : 0;
    const bool hasC = src < inputBytes;
    const uint32_t c = hasC ? input[src++] : 0;
    output[dst++] = kBase64[(a >> 2) & 0x3F];
    output[dst++] = kBase64[((a & 0x03) << 4) | (b >> 4)];
    output[dst++] = hasB ? kBase64[((b & 0x0F) << 2) | (c >> 6)] : '=';
    output[dst++] = hasC ? kBase64[c & 0x3F] : '=';
  }
  return dst;
}

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

size_t base64Decode(const char* input, uint8_t* output, size_t outputCap) {
  size_t dst = 0;
  int bits = 0;
  uint32_t acc = 0;
  for (const char* p = input; p != nullptr && *p != '\0'; ++p) {
    if (*p == '=') break;
    const int value = base64Value(*p);
    if (value < 0) continue;
    acc = (acc << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (dst == outputCap) return 0;
      output[dst++] = static_cast<uint8_t>((acc >> bits) & 0xFF);
    }
  }
  return dst;
}

void writeLe32(fs::File& file, uint32_t value) {
  uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                      static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  file.write(bytes, sizeof(bytes));
}

bool writePcmWavHeader(fs::File& file, uint32_t rate, uint32_t dataBytes) {
  if (!file.seek(0)) return false;
  file.write(reinterpret_cast<const uint8_t*>("RIFF"), 4);
  writeLe32(file, 36 + dataBytes);
  file.write(reinterpret_cast<const uint8_t*>("WAVEfmt "), 8);
  writeLe32(file, 16);
  const uint8_t format[] = {1, 0, 1, 0};  // PCM, mono
  file.write(format, sizeof(format));
  writeLe32(file, rate);
  writeLe32(file, rate * 2);
  const uint8_t alignment[] = {2, 0, 16, 0};
  file.write(alignment, sizeof(alignment));
  file.write(reinterpret_cast<const uint8_t*>("data"), 4);
  writeLe32(file, dataBytes);
  return file.position() == 44;
}

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

// Write-only Stream over a caller-owned buffer, so HTTPClient::writeToStream()
// can land a response body wherever we want it (here: PSRAM). Stream extends
// Print, so the read half has to exist to satisfy the interface; nothing ever
// calls it.
//
// It never writes past cap_, and it always REPORTS a full write even after
// saturating. That second part is load-bearing: a short write makes
// writeToStreamDataBlock() give up with HTTPC_ERROR_STREAM_WRITE, and
// returnError() then calls _client->stop() — tearing down the single keep-alive
// connection this whole exchange is built around. Overflow bytes are dropped on
// the floor, the socket still drains cleanly, and overflow_ records the loss.
class BufferSink : public Stream {
 public:
  BufferSink(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

  size_t write(uint8_t b) override {
    if (len_ < cap_) {
      buf_[len_++] = static_cast<char>(b);
    } else {
      overflow_ = true;
    }
    return 1;  // never short: see class comment
  }

  size_t write(const uint8_t* data, size_t size) override {
    const size_t room = cap_ - len_;
    const size_t n = size < room ? size : room;
    if (n > 0) {
      memcpy(buf_ + len_, data, n);
      len_ += n;
    }
    if (n < size) {
      overflow_ = true;
    }
    return size;  // never short: see class comment
  }

  // Read side: unused, present only because Stream declares it pure virtual.
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t length() const { return len_; }
  bool overflowed() const { return overflow_; }

 private:
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool overflow_ = false;
};

// Copies a trimmed HTTP error body into out for diagnostics (safe: OpenAI
// error JSON, never the request headers).
// This is the file's one remaining getString(), and it stays: it runs only on a
// non-200, error bodies are small, and it truncates to 48 chars anyway — the
// internal-RAM blowup that forced phase 2 onto a PSRAM sink cannot happen here.
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
  // Transcribe-only is deliberately kept on the file API even when Realtime
  // is selected: that command promises a file transcription, not an assistant
  // reply. Voice and text exchanges use the selected backend.
  if (self->settings_->assistantBackend() == AssistantBackend::Realtime &&
      self->mode_ != AssistantService::Mode::TranscribeOnly) {
    self->realtimeWorkerBody();
    return;
  }
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

  // Phase 2: Responses API with short history, optional built-in web search.
  if (!self->workerFailed_) {
    self->workerPhase_ = static_cast<uint8_t>(AssistantService::State::Thinking);
    JsonDocument req;
    req["model"] = kResponseModel;
    // `instructions` carries the system prompt on this endpoint, so `input`
    // holds conversation turns only — no system entry.
    req["instructions"] = kInstructions;
    req["max_output_tokens"] = kMaxAnswerTokens;
    req["store"] = kStoreResponses;  // opt out of server-side retention
    JsonArray input = req["input"].to<JsonArray>();
    for (uint8_t i = 0; i < self->historyCount_; i++) {
      JsonObject u = input.add<JsonObject>();
      u["role"] = "user";
      u["content"] = self->historySlot(i, 0);
      JsonObject a = input.add<JsonObject>();
      a["role"] = "assistant";
      a["content"] = self->historySlot(i, 1);
    }
    JsonObject cur = input.add<JsonObject>();
    cur["role"] = "user";
    cur["content"] = self->transcript_;
    // Both blocks below omit their key entirely when switched off — an empty
    // tools array or a null reasoning object is not the same request.
    if (kWebSearchEnabled) {
      JsonObject tool = req["tools"].to<JsonArray>().add<JsonObject>();
      tool["type"] = kWebSearchTool;
      req["tool_choice"] = "auto";  // only meaningful alongside tools
    }
    if (kReasoningEffort[0] != '\0') {
      req["reasoning"]["effort"] = kReasoningEffort;
    }
    String body;
    serializeJson(req, body);

    beginHttps(http, client, "https://api.openai.com/v1/responses", key, false);
    http.setTimeout(kResponsesTimeoutMs);  // overrides beginHttps: see constant
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (code < 0) {
      vTaskDelay(pdMS_TO_TICKS(750));  // transport hiccup: one retry
      code = http.POST(body);
    }
    if (code != 200) {
      char snip[48] = "";
      if (code > 0) snipBody(http, snip, sizeof(snip));
      snprintf(self->lastError_, sizeof(self->lastError_), "responses %d %s", code, snip);
      self->workerFailed_ = true;
    } else {
      // Read the body into PSRAM, NOT into an Arduino String: getString()
      // buffers the whole thing in internal RAM, and a web-search-backed answer
      // is far bigger than the plain completion this was written for.
      //
      // writeToStream() (never useHTTP10(true) — that forces HTTP/1.0, closes
      // the socket and costs a second TLS handshake before TTS, which is
      // exactly the fragmentation the single connection exists to prevent)
      // handles Content-Length and chunked framing alike and leaves the
      // connection reusable.
      char* buf = static_cast<char*>(
          heap_caps_malloc(kResponseBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (buf == nullptr) {
        snprintf(self->lastError_, sizeof(self->lastError_), "out of memory");
        self->workerFailed_ = true;
      } else {
        BufferSink sink(buf, kResponseBufferBytes);
        const int sunk = http.writeToStream(&sink);
        // Printed every exchange so a walk-down across web-search answers is
        // visible over serial rather than inferred after the fact.
        Serial.printf(
            "[assistant] response body %u bytes%s (writeToStream %d) — internal heap %u KB free "
            "(largest %u KB)\n",
            (unsigned)sink.length(), sink.overflowed() ? " TRUNCATED" : "", sunk,
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));

        if (sink.overflowed()) {
          // Report this BEFORE parsing. A truncated body is malformed JSON, and
          // "responses parse failed" would send the field chasing an API bug
          // that is not there — the buffer simply ran out.
          snprintf(self->lastError_, sizeof(self->lastError_), "response too large");
          self->workerFailed_ = true;
        } else {
          // The filter is load-bearing: with web search on, the raw body carries
          // search calls, queries and citation blocks, and parsing it whole
          // exhausts RAM. It must keep the `type` discriminators or a
          // web_search_call is indistinguishable from a message. In an array
          // filter, element 0 applies to every element.
          JsonDocument filter;
          filter["output"][0]["type"] = true;
          filter["output"][0]["content"][0]["type"] = true;
          filter["output"][0]["content"][0]["text"] = true;
          filter["error"] = true;  // a 200 can still carry a structured error
          JsonDocument doc;
          // Non-const char* + length picks ArduinoJson's zero-copy mode: the
          // document points into buf instead of duplicating every string into
          // itself. buf therefore has to outlive the walk below — which is why
          // the free sits at the bottom of this block, not here.
          if (deserializeJson(doc, buf, sink.length(),
                              DeserializationOption::Filter(filter)) !=
              DeserializationError::Ok) {
            snprintf(self->lastError_, sizeof(self->lastError_), "responses parse failed");
            self->workerFailed_ = true;
          } else {
            // `output_text` is an SDK convenience and is NOT in the raw JSON: walk
            // `output` ourselves. Message items are interleaved with
            // web_search_call items, and a message may hold several text parts.
            size_t used = 0;
            self->answer_[0] = '\0';
            for (JsonObjectConst item : doc["output"].as<JsonArrayConst>()) {
              if (used >= sizeof(self->answer_) - 1) {
                break;
              }
              const char* itemType = item["type"];
              if (itemType == nullptr || strcmp(itemType, "message") != 0) {
                continue;  // web_search_call, reasoning, anything else
              }
              for (JsonObjectConst part : item["content"].as<JsonArrayConst>()) {
                const char* partType = part["type"];
                if (partType == nullptr || strcmp(partType, "output_text") != 0) {
                  continue;  // refusal, annotations-only part, ...
                }
                const char* text = part["text"];
                if (text == nullptr) {
                  continue;
                }
                const size_t room = sizeof(self->answer_) - 1 - used;
                if (room == 0) {
                  break;
                }
                const size_t n = strnlen(text, room);
                memcpy(self->answer_ + used, text, n);
                used += n;
                self->answer_[used] = '\0';
              }
            }
            if (used == 0) {
              // Deliberately distinct from "responses parse failed": well-formed
              // JSON with no assistant text is a different bug from malformed
              // JSON, and this is the only diagnostic the field gets.
              const char* apiError = doc["error"]["message"];
              if (apiError != nullptr) {
                snprintf(self->lastError_, sizeof(self->lastError_), "responses error: %s",
                         apiError);
              } else {
                snprintf(self->lastError_, sizeof(self->lastError_), "no answer in response");
              }
              self->workerFailed_ = true;
            }
          }
        }
        // One exit for the buffer: every branch above (overflow, parse failure,
        // no-answer, success) falls through to here, and `doc` is dead by now.
        heap_caps_free(buf);
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

void AssistantService::realtimeWorkerBody() {
  workerFailed_ = false;
  workerPhase_ = static_cast<uint8_t>(State::Thinking);
  answer_[0] = '\0';
  if (mode_ == Mode::Text) {
    strncpy(transcript_, textQuery_, sizeof(transcript_) - 1);
    transcript_[sizeof(transcript_) - 1] = '\0';
  } else {
    transcript_[0] = '\0';
  }

  // Realtime output is raw 24 kHz PCM. Keep it behind the same atomic WAV
  // handoff as classic TTS, so the loop task remains the sole owner of I2S.
  const String partialPath = AtomicFile::partialPath(kReplyPath);
  SD_MMC.remove(partialPath);
  fs::File reply = SD_MMC.open(partialPath, FILE_WRITE);
  if (!reply || !writePcmWavHeader(reply, kRealtimePcmRate, 0)) {
    if (reply) reply.close();
    snprintf(lastError_, sizeof(lastError_), "realtime reply file failed");
    workerFailed_ = true;
    return;
  }

  // This board captures 16 kHz PCM, while the current Realtime PCM schema
  // uses 24 kHz. Expanding each 16-bit pair to a, a, b preserves duration and
  // pitch without allocating the whole take; interpolation can replace this
  // compact first pass if listening tests reveal an audible need.
  constexpr size_t kInputBytes = 4096;
  constexpr size_t kUpsampledBytes = kInputBytes * 3 / 2;
  constexpr size_t kEventBytes = 48 + ((kUpsampledBytes + 2) / 3) * 4 + 4;
  // The WebSocket dependency accepts up to 32 KB base64 JSON frames. Reserve
  // enough PSRAM for their decoded PCM payload rather than discarding a valid
  // first audio delta that exceeds the old 12 KB scratch buffer.
  constexpr size_t kDecodeBytes = 24 * 1024;
  uint8_t* input = static_cast<uint8_t*>(
      heap_caps_malloc(kInputBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* upsampled = static_cast<uint8_t*>(
      heap_caps_malloc(kUpsampledBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  char* event = static_cast<char*>(
      heap_caps_malloc(kEventBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t* decoded = static_cast<uint8_t*>(
      heap_caps_malloc(kDecodeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (input == nullptr || upsampled == nullptr || event == nullptr || decoded == nullptr) {
    if (input) heap_caps_free(input);
    if (upsampled) heap_caps_free(upsampled);
    if (event) heap_caps_free(event);
    if (decoded) heap_caps_free(decoded);
    reply.close();
    SD_MMC.remove(partialPath);
    snprintf(lastError_, sizeof(lastError_), "realtime out of memory");
    workerFailed_ = true;
    return;
  }

  WebSocketsClient socket;
  bool connected = false;
  bool sessionCreated = false;
  bool configured = false;
  bool responseDone = false;
  bool replyWriteFailed = false;
  bool closingSocket = false;
  uint32_t replyBytes = 0;

  socket.onEvent([&](WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_CONNECTED) {
      connected = true;
      Serial.println("[assistant] realtime WebSocket connected");
      return;
    }
    if (type == WStype_DISCONNECTED || type == WStype_ERROR) {
      if (closingSocket) return;
      if (!workerFailed_) {
        const size_t copy = min(length, sizeof(lastError_) - 20);
        snprintf(lastError_, sizeof(lastError_), "realtime socket %s: %.*s",
                 type == WStype_DISCONNECTED ? "closed" : "error", static_cast<int>(copy),
                 payload != nullptr ? reinterpret_cast<const char*>(payload) : "no detail");
      }
      workerFailed_ = true;
      responseDone = true;
      return;
    }
    if (type != WStype_TEXT || payload == nullptr || length == 0) {
      return;
    }
    JsonDocument filter;
    filter["type"] = true;
    filter["delta"] = true;
    filter["transcript"] = true;
    filter["error"]["message"] = true;
    JsonDocument message;
    if (deserializeJson(message, payload, length, DeserializationOption::Filter(filter)) !=
        DeserializationError::Ok) {
      return;  // ignore unknown/oversized observability events
    }
    const char* eventType = message["type"];
    if (eventType == nullptr) return;
    Serial.printf("[assistant] realtime event: %s\n", eventType);
    if (strcmp(eventType, "session.created") == 0) {
      sessionCreated = true;
    } else if (strcmp(eventType, "session.updated") == 0) {
      configured = true;
    } else if (strcmp(eventType, "error") == 0) {
      const char* detail = message["error"]["message"];
      snprintf(lastError_, sizeof(lastError_), "realtime: %s", detail != nullptr ? detail : "error");
      Serial.printf("[assistant] realtime server error: %s\n",
                    detail != nullptr ? detail : "no message");
      workerFailed_ = true;
      responseDone = true;
    } else if (strcmp(eventType, "conversation.item.input_audio_transcription.completed") == 0) {
      const char* text = message["transcript"];
      if (text != nullptr) {
        strncpy(transcript_, text, sizeof(transcript_) - 1);
        transcript_[sizeof(transcript_) - 1] = '\0';
      }
    } else if (strcmp(eventType, "response.output_audio_transcript.delta") == 0) {
      const char* delta = message["delta"];
      if (delta != nullptr) {
        const size_t used = strlen(answer_);
        if (used + 1 < sizeof(answer_)) {
          strncat(answer_, delta, sizeof(answer_) - used - 1);
        }
      }
    } else if (strcmp(eventType, "response.output_audio.delta") == 0) {
      const char* delta = message["delta"];
      if (delta == nullptr || replyWriteFailed) return;
      const size_t bytes = base64Decode(delta, decoded, kDecodeBytes);
      if (bytes == 0 || reply.write(decoded, bytes) != bytes) {
        replyWriteFailed = true;
      } else {
        replyBytes += bytes;
      }
    } else if (strcmp(eventType, "response.done") == 0) {
      responseDone = true;
    }
  });

  // This matches the existing assistant's documented no-cert-store policy.
  // The WebSockets library calls setInsecure() when no CA bundle is supplied.
  // WebSocketsClient appends the terminating CRLF itself. Supplying one here
  // ended the handshake headers early and left its User-Agent outside them.
  const String headers = String("Authorization: Bearer ") + settings_->openaiKey();
  socket.setExtraHeaders(headers.c_str());
  socket.setReconnectInterval(5000);
  socket.beginSSL(kRealtimeHost, kRealtimePort,
                  String("/v1/realtime?model=") + kRealtimeModel, String(""), String(""));

  const uint32_t connectStarted = millis();
  while (!connected && millis() - connectStarted < kRealtimeConnectTimeoutMs) {
    socket.loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!connected) {
    if (!workerFailed_) snprintf(lastError_, sizeof(lastError_), "realtime WebSocket timeout");
    workerFailed_ = true;
  }

  // The endpoint creates a Realtime session after the WebSocket upgrade.
  // Do not race session.update ahead of that lifecycle event.
  const uint32_t sessionStarted = millis();
  while (!workerFailed_ && !sessionCreated &&
         millis() - sessionStarted < kRealtimeConnectTimeoutMs) {
    socket.loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!workerFailed_ && !sessionCreated) {
    snprintf(lastError_, sizeof(lastError_), "realtime session creation timeout");
    workerFailed_ = true;
  }

  const char* sessionUpdate =
      "{\"type\":\"session.update\",\"session\":{\"type\":\"realtime\","
      "\"output_modalities\":[\"audio\"],\"audio\":{\"input\":{\"format\":{"
      "\"type\":\"audio/pcm\",\"rate\":24000},\"turn_detection\":null},"
      "\"output\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},\"voice\":\"alloy\"}},"
      "\"instructions\":\"You are a concise voice assistant on an ESP32 device. "
      "Use plain spoken language and keep answers to one to three short sentences.\"}}";
  if (!workerFailed_ && !socket.sendTXT(sessionUpdate)) {
    snprintf(lastError_, sizeof(lastError_), "realtime session update failed");
    workerFailed_ = true;
  }
  const uint32_t configureStarted = millis();
  while (!workerFailed_ && !configured && millis() - configureStarted < kRealtimeConnectTimeoutMs) {
    socket.loop();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!workerFailed_ && !configured) {
    snprintf(lastError_, sizeof(lastError_), "realtime session timeout");
    workerFailed_ = true;
  }

  if (!workerFailed_ && mode_ == Mode::Text) {
    JsonDocument textEvent;
    textEvent["type"] = "conversation.item.create";
    JsonObject item = textEvent["item"].to<JsonObject>();
    item["type"] = "message";
    item["role"] = "user";
    item["content"].to<JsonArray>().add<JsonObject>()["type"] = "input_text";
    item["content"][0]["text"] = textQuery_;
    String serialized;
    serializeJson(textEvent, serialized);
    if (!socket.sendTXT(serialized) || !socket.sendTXT("{\"type\":\"response.create\"}")) {
      snprintf(lastError_, sizeof(lastError_), "realtime text send failed");
      workerFailed_ = true;
    }
  } else if (!workerFailed_) {
    fs::File source = SD_MMC.open(kQueryPath, FILE_READ);
    if (!source || source.size() <= 44 || !source.seek(44)) {
      if (source) source.close();
      snprintf(lastError_, sizeof(lastError_), "no captured audio");
      workerFailed_ = true;
    } else {
      while (!workerFailed_ && source.available() > 0) {
        size_t sourceBytes = source.read(input, kInputBytes);
        sourceBytes &= ~static_cast<size_t>(3);  // whole 16-bit sample pairs only
        if (sourceBytes == 0) break;
        size_t outputBytes = 0;
        for (size_t i = 0; i < sourceBytes; i += 4) {
          memcpy(upsampled + outputBytes, input + i, 2);
          memcpy(upsampled + outputBytes + 2, input + i, 2);
          memcpy(upsampled + outputBytes + 4, input + i + 2, 2);
          outputBytes += 6;
        }
        static constexpr char kPrefix[] = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
        memcpy(event, kPrefix, sizeof(kPrefix) - 1);
        const size_t encoded = base64Encode(upsampled, outputBytes, event + sizeof(kPrefix) - 1);
        const size_t eventLength = sizeof(kPrefix) - 1 + encoded;
        event[eventLength] = '\"';
        event[eventLength + 1] = '}';
        event[eventLength + 2] = '\0';
        if (!socket.sendTXT(event, eventLength + 2)) {
          snprintf(lastError_, sizeof(lastError_), "realtime audio send failed");
          workerFailed_ = true;
          break;
        }
        socket.loop();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      source.close();
      if (!workerFailed_ &&
          (!socket.sendTXT("{\"type\":\"input_audio_buffer.commit\"}") ||
           !socket.sendTXT("{\"type\":\"response.create\"}"))) {
        snprintf(lastError_, sizeof(lastError_), "realtime response start failed");
        workerFailed_ = true;
      }
    }
  }

  const uint32_t responseStarted = millis();
  while (!workerFailed_ && !responseDone &&
         millis() - responseStarted < kRealtimeResponseTimeoutMs) {
    socket.loop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (!workerFailed_ && !responseDone) {
    snprintf(lastError_, sizeof(lastError_), "realtime response timeout");
    workerFailed_ = true;
  }
  closingSocket = true;
  socket.disconnect();

  if (replyWriteFailed || replyBytes == 0) {
    if (!workerFailed_) snprintf(lastError_, sizeof(lastError_), "realtime audio write failed");
    workerFailed_ = true;
  }
  if (!workerFailed_ && answer_[0] == '\0') {
    snprintf(lastError_, sizeof(lastError_), "realtime reply had no transcript");
    workerFailed_ = true;
  }
  if (transcript_[0] == '\0') {
    strncpy(transcript_, "voice request", sizeof(transcript_) - 1);
    transcript_[sizeof(transcript_) - 1] = '\0';
  }
  if (!workerFailed_ && !writePcmWavHeader(reply, kRealtimePcmRate, replyBytes)) {
    snprintf(lastError_, sizeof(lastError_), "realtime WAV finalize failed");
    workerFailed_ = true;
  }
  reply.close();
  if (!workerFailed_) {
    if (!AtomicFile::finalizePartial(SD_MMC, kReplyPath)) {
      snprintf(lastError_, sizeof(lastError_), "realtime SD finalize failed");
      workerFailed_ = true;
    }
  }
  if (workerFailed_) {
    SD_MMC.remove(partialPath);
  }
  heap_caps_free(input);
  heap_caps_free(upsampled);
  heap_caps_free(event);
  heap_caps_free(decoded);
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
