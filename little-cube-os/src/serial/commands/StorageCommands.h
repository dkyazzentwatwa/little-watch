#pragma once

struct Services;

// `storage ...` family (spec §17): status/mount/eject/usage/index/backup.
bool handleStorageCommand(Services& services, const char* verb, char* args);
void printStorageHelp();
