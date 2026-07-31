#pragma once

struct Services;

// `audio ...` family + `volume` (spec §17). Radio has its own `radio ...`
// family because stations are network presets, not local audio files.
bool handleAudioCommand(Services& services, const char* verb, char* args);
bool handleVolumeCommand(Services& services, char* args);
void printAudioHelp();
bool handleRadioCommand(Services& services, const char* verb, char* args);
void printRadioHelp();
