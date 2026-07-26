#pragma once

struct Services;

// `audio ...` family + `volume` (spec §17). v1 plays WAV recordings; the
// music/podcast/radio library arrives with the audio-depth follow-up.
bool handleAudioCommand(Services& services, const char* verb, char* args);
bool handleVolumeCommand(Services& services, char* args);
void printAudioHelp();
