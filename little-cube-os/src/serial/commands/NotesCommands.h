#pragma once

struct Services;
class MultilineBuffer;

// `notes ...` family (spec §17). Multiline verbs (new/write/append) arm the
// shared MultilineBuffer; the dispatcher pumps subsequent lines into it.
bool handleNotesCommand(Services& services, MultilineBuffer& multiline, const char* verb,
                        char* args);
void printNotesHelp();
