#pragma once

// Storage serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerStorageCommands(SerialCommandService& serial, Services& services);
