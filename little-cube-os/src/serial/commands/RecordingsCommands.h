#pragma once

struct Services;

// `recordings ...` family (spec §17): list/rename/delete/info.
bool handleRecordingsCommand(Services& services, const char* verb, char* args);
void printRecordingsHelp();
