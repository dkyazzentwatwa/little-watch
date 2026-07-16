#pragma once

// Wifi serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerWifiCommands(SerialCommandService& serial, Services& services);
