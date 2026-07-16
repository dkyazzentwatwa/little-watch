#pragma once

// Files serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerFilesCommands(SerialCommandService& serial, Services& services);
