#pragma once

struct Services;

// `calendar ...` family (spec §14, §17). Same shape as every other
// implemented family: the dispatcher parses `family verb args...` and calls
// this with the verb and the remaining (mutable) argument buffer. Returns
// false only for an unknown verb, so the dispatcher can print the error.
//
// Verbs that take an <id> act on the day shown by the last `calendar list`
// (the loaded day of CalendarService). Destructive verbs require a literal
// `confirm` token (spec §44).
bool handleCalendarCommand(Services& services, const char* verb, char* args);
void printCalendarHelp();
