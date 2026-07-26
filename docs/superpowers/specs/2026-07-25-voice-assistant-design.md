# Voice Assistant (OpenAI) — Design

Date: 2026-07-25 · Status: approved (chat) · Owner: cypher

## Goal

Talk to the cube: record a spoken question, transcribe it with the OpenAI API,
answer it with a chat model, synthesize the answer with OpenAI TTS, and play it
through the speaker. Multi-turn: follow-up questions see recent context.

## Decisions (user-confirmed)

- **Pipeline**: three calls — STT → chat → TTS (no audio-preview single call,
  no Realtime/WebSocket in v1).
- **Memory**: multi-turn, last 6 exchanges, RAM only; cleared on app exit,
  `assistant reset`, or reboot.
- **Placement**: new dedicated **Assistant** app on the carousel.
- **Key**: user has an OpenAI API key; entered by the user directly over the
  serial console via a masked prompt. Never in chat, never printed, never on SD.

## Architecture

New pieces, following existing repo patterns:

| Unit | Purpose | Depends on |
|---|---|---|
| `services/AssistantService.{h,cpp}` | State machine + one FreeRTOS worker task per exchange; OpenAI HTTP client lives in its .cpp (anonymous namespace) | AudioAdapter, WifiService, SettingsService, SdStorage, SystemState |
| `apps/AssistantApp.{h,cpp}` | Talk button, live status, last transcript + answer, error display | AssistantService, Theme/StatusBar |
| `serial/commands/AssistantCommands.{h,cpp}` | `assistant key/ask/transcribe/reset/status` family | AssistantService, SettingsService |
| SettingsService addition | `openaiKey()` / `setOpenaiKey()` in NVS (`Preferences`), like other settings; key string never echoed | — |
| `AppId::Assistant` + registry + `appName()` | 13th app | — |

State machine (`AssistantService::State`, loop-task owned like AudioAdapter's
contract): `Idle → Listening → Transcribing → Thinking → Speaking → Idle`, plus
`Error` (holds a short human-readable message until the next action). The
worker task communicates via `volatile` results + a done semaphore; the state
leaves in-flight states only in `update()` after taking the semaphore. Workers
never touch UI; the app polls service state in `update()` and marks itself
dirty on change (no new EventBus events).

## Data flow

1. **Listen**: tap talk button (or `assistant voice` over serial) →
   `AudioAdapter::startRecordWav("/littlecube/assistant/query.wav.partial",
   16000)`. The existing capture path applies: 400 ms discard, noise gate,
   −3 dBFS normalize — all good for STT. Auto-stop at 30 s. Kernel capture
   quieting (PMU/RTC pause, SD-probe deferral) extends to assistant captures:
   condition becomes `recorderService.recording() || assistantService.capturing()`.
2. **Transcribe**: worker stages the WAV into a PSRAM buffer, builds one
   multipart body (PSRAM, ≤ ~1 MB), POSTs to
   `https://api.openai.com/v1/audio/transcriptions` (`model` constant, default
   `gpt-4o-mini-transcribe`; drop to `whisper-1` if bring-up shows 404), parses
   `{"text"}` with ArduinoJson.
3. **Think**: POST `/v1/chat/completions` — system prompt tuned for spoken
   answers (1–3 sentences, no markdown), history (≤ 6 exchanges, each side
   truncated to 1 KB), `max_tokens` 220, model constant `gpt-4o-mini`. Parse
   `choices[0].message.content`.
4. **Speak**: POST `/v1/audio/speech` (`gpt-4o-mini-tts`, voice `alloy`,
   `response_format":"wav"`) — stream body to
   `/littlecube/assistant/reply.wav.partial`, finalize, then
   `requestPlayWavFile()` (24 kHz mono WAV plays via the existing
   header-driven I2S reconfig).

HTTP: `WiFiClientSecure` + `setInsecure()` + `HTTPClient`, HTTP/1.0 forced when
streaming raw bodies — exactly the podcast-download pattern, including `.partial`
finalize and bounded chunk loops. 30 s timeouts per call. `Authorization:
Bearer <key>` header; the key travels only in that header.

Model names, system prompt, voice, caps: constants in one block at the top of
`AssistantService.cpp`.

## Errors & degradation

Every failure lands in `Error` with a short on-screen message and a fuller
serial line: no Wi-Fi ("offline — connect Wi-Fi first"), no key ("no API key —
run: assistant key"), no SD ("assistant needs the SD card"), HTTP != 200
(status + trimmed body snippet — never the key), timeout, empty transcript
("didn't catch that"). Back cancels a listening session without sending.
Half-duplex: recording refuses to start until playback is idle (existing
AudioAdapter gate). Boot works with the feature unconfigured.

## Security

- Key in NVS only (`Preferences`, namespace `littlecube`); masked serial entry
  (reuse the Wi-Fi password prompt mechanism, buffers zeroed after use);
  `assistant status` reports only "key: set / missing".
- TLS is `setInsecure()` — consistent with every network service in this repo;
  documented tradeoff (no server verification). Cert pinning for
  `api.openai.com` is a candidate hardening follow-up, out of v1 scope.

## Serial family

```
assistant key                 masked prompt; stores the OpenAI API key in NVS
assistant status              state, key set/missing, online, history depth
assistant ask "<text>"        skip STT: text → chat → TTS → speaker
assistant transcribe <path>   STT only, prints the transcript (pipeline test)
assistant voice               start/stop a full voice exchange from serial
assistant reset               clear conversation history
```

## Testing (hardware-validation rule applies: no checkmark without the cube)

1. Clean build; flash.
2. User enters key (`assistant key` typed by the user in their own console).
3. Autonomous over serial: `assistant status` → `assistant ask "Say the word
   hello"` (verifies chat + TTS + playback start + reply.wav on SD) →
   `assistant transcribe` against an existing voice note (verifies STT).
4. User: full tap-talk-listen loop, follow-up question (memory), Back-cancel.
5. Add (unchecked) items to `docs/hardware-validation.md`; check only what was
   physically exercised.

## Out of scope (v1)

Wake word, VAD auto-stop, streaming/Realtime API, barge-in, language setting,
non-OpenAI providers, cert pinning, persisting conversation history.
