#pragma once

struct Services;

// `contacts ...` family (spec §26, §17). Same shape as every other
// implemented family: the dispatcher parses `family verb args...` and calls
// this with the verb and the remaining (mutable) argument buffer. Returns
// false only for an unknown verb, so the dispatcher can print the error.
//
// Contacts are addressed by their id (shown by `contacts list`), never by a
// list position, so a stale listing cannot delete the wrong record.
// Destructive verbs require a literal `confirm` token (spec §44).
bool handleContactsCommand(Services& services, const char* verb, char* args);
void printContactsHelp();
