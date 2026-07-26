#pragma once

#include "WifiCommands.h"  // PasswordPrompt

struct Services;

// `assistant` serial family. The key prompt reuses the Wi-Fi PasswordPrompt
// mechanism: the NEXT line typed goes straight to NVS and is wiped from the
// line buffer — never echoed, never logged.
bool handleAssistantCommand(Services& services, PasswordPrompt& keyPrompt,
                            const char* verb, char* args);
void printAssistantHelp();
