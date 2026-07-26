#include "AssistantCommands.h"

#include <Arduino.h>

#include "../../core/Services.h"
#include "../../core/SystemState.h"
#include "../../services/AssistantService.h"
#include "../../services/SettingsService.h"
#include "../../storage/SdStorage.h"
#include "../CmdArgs.h"

void printAssistantHelp() {
  Serial.println("assistant status              state, key set/missing, memory depth");
  Serial.println("assistant key                 store the OpenAI API key (masked prompt)");
  Serial.println("assistant ask <question>      text question -> spoken answer");
  Serial.println("assistant voice               start / send a spoken question");
  Serial.println("assistant transcribe <path>   STT only on a WAV (pipeline test)");
  Serial.println("assistant reset               clear conversation memory");
}

bool handleAssistantCommand(Services& services, PasswordPrompt& keyPrompt,
                            const char* verb, char* args) {
  AssistantService* ai = services.assistant;
  if (ai == nullptr || services.settings == nullptr) {
    return false;
  }

  if (strcmp(verb, "status") == 0) {
    Serial.printf("state: %s, key: %s, online: %s, history: %u\n", ai->stateName(),
                  services.settings->hasOpenaiKey() ? "set" : "missing",
                  services.state != nullptr && services.state->internet ? "yes" : "no",
                  (unsigned)ai->historyDepth());
    if (ai->lastError()[0] != '\0') {
      Serial.printf("last error: %s\n", ai->lastError());
    }
    return true;
  }

  if (strcmp(verb, "key") == 0) {
    keyPrompt.active = true;
    keyPrompt.hidden = false;
    keyPrompt.ssid[0] = '\0';  // unused by the assistant prompt
    Serial.println("paste the OpenAI API key and press Enter (blank line cancels):");
    return true;
  }

  if (strcmp(verb, "ask") == 0) {
    char* cursor = args;
    const char* question = cmdargs::rest(cursor);
    if (question == nullptr || question[0] == '\0') {
      Serial.println("usage: assistant ask <question>");
      return true;
    }
    Serial.println(ai->askText(question) ? "thinking..."
                                         : "error: assistant busy or no key");
    return true;
  }

  if (strcmp(verb, "voice") == 0) {
    if (ai->state() == AssistantService::State::Listening) {
      ai->finishListening();
      Serial.println("sending...");
    } else if (ai->startListening()) {
      Serial.println("listening — `assistant voice` again to send");
    } else {
      Serial.printf("error: %s\n",
                    ai->lastError()[0] != '\0' ? ai->lastError() : "assistant busy");
    }
    return true;
  }

  if (strcmp(verb, "transcribe") == 0) {
    char* cursor = args;
    const char* raw = cmdargs::nextToken(cursor);
    String path;
    if (raw == nullptr || services.storage == nullptr ||
        !services.storage->sanitizePath(raw, path)) {
      Serial.println("usage: assistant transcribe </littlecube/.../file.wav>");
      return true;
    }
    Serial.println(ai->transcribeOnly(path.c_str()) ? "transcribing..."
                                                    : "error: assistant busy");
    return true;
  }

  if (strcmp(verb, "reset") == 0) {
    ai->resetHistory();
    Serial.println("conversation memory cleared");
    return true;
  }

  return false;
}
