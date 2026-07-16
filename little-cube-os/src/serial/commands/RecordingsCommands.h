#pragma once

// Recordings serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerRecordingsCommands(SerialCommandService& serial, Services& services);
