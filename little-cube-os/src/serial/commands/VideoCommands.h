#pragma once

#include "../../feature_flags.h"

#if FEATURE_VIDEO

struct Services;

// `video ...` family: list/play/pause/resume/seek/stop/status/queue.
// Commands act through VideoService/VideoPlayer, never on hardware.
bool handleVideoCommand(Services& services, const char* verb, char* args);
void printVideoHelp();

#endif  // FEATURE_VIDEO
