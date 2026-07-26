#pragma once

struct Services;

// `settings ...` family (spec §17, §28). Same shape as every other
// implemented family: the dispatcher parses `family verb args...` and calls
// this with the verb and the remaining (mutable) argument buffer. Returns
// false only for an unknown verb, so the dispatcher can print the error.
//
// SettingsService clamps every value on the way in (and again on the way out
// of NVS), so these verbs never pre-validate a range — they set, then read
// back and report what was ACTUALLY stored. A silently-adjusted value is a
// lie; a printed one is a range hint.
//
// This is the only editor anywhere for the bedtime window and its brightness:
// the Settings app has room for the on/off toggle and nothing else.
bool handleSettingsCommand(Services& services, const char* verb, char* args);
void printSettingsHelp();
