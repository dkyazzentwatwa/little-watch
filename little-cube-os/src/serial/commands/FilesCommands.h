#pragma once

struct Services;

// `files ...` family (spec §17): list/tree/cat/mkdir/copy/move/rename/
// delete, all path-sanitized to /littlecube and the deck interop tree.
bool handleFilesCommand(Services& services, const char* verb, char* args);
void printFilesHelp();
