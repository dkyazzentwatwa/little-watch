#pragma once

struct Services;

// `news ...` family (spec §16-18 shape): read the fetched/cached headlines and
// trigger a pull. All work goes through NewsService — this never touches the
// network or filesystem directly.
bool handleNewsCommand(Services& services, const char* verb, char* args);
void printNewsHelp();
