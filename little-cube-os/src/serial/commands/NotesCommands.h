#pragma once

// Notes serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerNotesCommands(SerialCommandService& serial, Services& services);
