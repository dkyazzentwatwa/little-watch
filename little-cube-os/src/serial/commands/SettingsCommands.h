#pragma once

// Settings serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerSettingsCommands(SerialCommandService& serial, Services& services);
