#pragma once

// Audio serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerAudioCommands(SerialCommandService& serial, Services& services);
